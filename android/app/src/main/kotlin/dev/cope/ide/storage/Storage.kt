// Storage access, and the honest story about it.
//
// Android gives three different worlds and Cope uses all three, in this order of
// preference, because each one buys something real:
//
//  1. REAL PATHS with all-files access (API 30+ MANAGE_EXTERNAL_STORAGE, or the
//     legacy permission on 27-29). The only mode where a file is *mmapped* by the
//     native host — a 200 MB file opens instantly and costs no heap. It is also
//     the only mode where the file tree can browse, because the native listDir is
//     a real opendir.
//  2. SAF single documents (always available, no permission prompt). The document
//     is read whole into memory and written back through the ContentResolver. The
//     UI states this, because it is why a huge SAF file is refused.
//  3. The app's own private directory, which always works and is where new
//     unsaved buffers get saved if the user has granted nothing.
//
// Nothing here silently degrades: the caller learns which mode it got.
package dev.cope.ide.storage

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.provider.Settings
import java.io.File
import java.io.IOException

public enum class StorageMode { ALL_FILES, SAF_ONLY }

/** Outcome of a mutating file operation: the new path, or the real reason it failed. */
public class FileResult private constructor(
    public val path: String?,
    public val error: String?,
) {
    public companion object {
        public fun ok(path: String): FileResult = FileResult(path, null)
        public fun failed(reason: String): FileResult = FileResult(null, reason)
    }
}

public object Storage {

    /** Bytes we refuse to pull through a ContentResolver in one go. */
    public const val SAF_BYTE_LIMIT: Long = 32L * 1024L * 1024L

    public fun mode(context: Context): StorageMode =
        if (hasAllFilesAccess(context)) StorageMode.ALL_FILES else StorageMode.SAF_ONLY

    public fun hasAllFilesAccess(context: Context): Boolean =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            Environment.isExternalStorageManager()
        } else {
            context.checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) ==
                android.content.pm.PackageManager.PERMISSION_GRANTED
        }

    /** Settings screen for all-files access; null below API 30. */
    public fun allFilesAccessIntent(context: Context): Intent? {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return null
        return Intent(
            Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
            Uri.parse("package:${context.packageName}"),
        )
    }

    public fun defaultRoot(): String =
        @Suppress("DEPRECATION")
        Environment.getExternalStorageDirectory()?.absolutePath ?: "/"

    /** Display name of a SAF document, falling back to the last path segment. */
    public fun displayName(context: Context, uri: Uri): String {
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
                ?.use { cursor ->
                    val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (index >= 0 && cursor.moveToFirst()) {
                        val name = cursor.getString(index)
                        if (!name.isNullOrEmpty()) return name
                    }
                }
        } catch (error: RuntimeException) {
            // A provider that dies on query is not our problem to solve; fall
            // through to the path segment.
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "untitled"
    }

    public fun sizeOf(context: Context, uri: Uri): Long {
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)
                ?.use { cursor ->
                    val index = cursor.getColumnIndex(OpenableColumns.SIZE)
                    if (index >= 0 && cursor.moveToFirst() && !cursor.isNull(index)) {
                        return cursor.getLong(index)
                    }
                }
        } catch (error: RuntimeException) {
            return -1
        }
        return -1
    }

    /** Reads a SAF document whole. Null on failure or past [SAF_BYTE_LIMIT]. */
    public fun readAll(context: Context, uri: Uri): ByteArray? {
        val size = sizeOf(context, uri)
        if (size > SAF_BYTE_LIMIT) return null
        return try {
            context.contentResolver.openInputStream(uri)?.use { stream ->
                stream.readBytes()
            }
        } catch (error: IOException) {
            null
        } catch (error: SecurityException) {
            null
        }
    }

    /** Writes back to a SAF document. "wt" truncates, which is what a save means. */
    public fun writeAll(context: Context, uri: Uri, bytes: ByteArray): Boolean = try {
        context.contentResolver.openOutputStream(uri, "wt")?.use { stream ->
            stream.write(bytes)
            stream.flush()
            true
        } ?: false
    } catch (error: IOException) {
        false
    } catch (error: SecurityException) {
        false
    }

    /** Keeps read/write access to a picked document across process death. */
    public fun persist(context: Context, uri: Uri) {
        try {
            context.contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION,
            )
        } catch (error: SecurityException) {
            // The provider did not offer a persistable grant. The document still
            // works for this session.
        }
    }

    /**
     * Real filesystem path of a SAF tree/document URI when one can be derived.
     * Best effort by design: when it works the file can be mmapped instead of
     * copied, and when it does not the caller falls back to reading bytes.
     */
    public fun realPathOf(uri: Uri): String? {
        if (uri.authority != "com.android.externalstorage.documents") return null
        val documentId = try {
            if (DocumentsContract.isTreeUri(uri)) {
                DocumentsContract.getTreeDocumentId(uri)
            } else {
                DocumentsContract.getDocumentId(uri)
            }
        } catch (error: IllegalArgumentException) {
            return null
        }
        val parts = documentId.split(':', limit = 2)
        if (parts.size != 2 || parts[0] != "primary") return null
        val root = defaultRoot()
        return if (parts[1].isEmpty()) root else "$root/${parts[1]}"
    }

    // --- mutating operations ------------------------------------------------
    //
    // Only reachable in ALL_FILES mode. Every failure returns the real reason
    // ("a folder of that name already exists") rather than a generic message,
    // because the design brief forbids "something went wrong".

    /** Legal in a FAT/exFAT-backed name, which /sdcard often is. */
    private const val ILLEGAL_NAME_CHARS = "/\\:*?\"<>|"

    private fun validateName(name: String): String? = when {
        name.isEmpty() -> "A name cannot be empty."
        name == "." || name == ".." -> "\"$name\" is a directory reference, not a name."
        name.any { it in ILLEGAL_NAME_CHARS } -> "A name cannot contain any of $ILLEGAL_NAME_CHARS"
        name.length > 255 -> "That name is ${name.length} characters; the limit is 255."
        else -> null
    }

    public fun rename(path: String, newName: String): FileResult {
        validateName(newName)?.let { return FileResult.failed(it) }
        val source = File(path)
        if (!source.exists()) return FileResult.failed("$path no longer exists.")
        if (source.name == newName) return FileResult.ok(path)
        val target = File(source.parentFile, newName)
        if (target.exists()) {
            return FileResult.failed(
                "${target.name} already exists in this folder. Pick another name.",
            )
        }
        return try {
            if (source.renameTo(target)) {
                FileResult.ok(target.absolutePath)
            } else {
                FileResult.failed(
                    "Android refused to rename ${source.name}. The folder is probably read-only " +
                        "or on a volume this app cannot write.",
                )
            }
        } catch (error: SecurityException) {
            FileResult.failed("No permission to rename ${source.name}.")
        }
    }

    public fun create(directory: String, name: String, folder: Boolean): FileResult {
        validateName(name)?.let { return FileResult.failed(it) }
        val parent = File(directory)
        if (!parent.isDirectory) return FileResult.failed("$directory is not a folder.")
        val target = File(parent, name)
        if (target.exists()) {
            return FileResult.failed("${target.name} already exists in this folder.")
        }
        return try {
            val created = if (folder) target.mkdir() else target.createNewFile()
            if (created) {
                FileResult.ok(target.absolutePath)
            } else {
                FileResult.failed(
                    "Android refused to create ${target.name} in ${parent.name}. " +
                        "That folder is not writable by this app.",
                )
            }
        } catch (error: IOException) {
            FileResult.failed("Could not create ${target.name}: ${error.message ?: "I/O error"}.")
        } catch (error: SecurityException) {
            FileResult.failed("No permission to write in ${parent.name}.")
        }
    }

    /** Deletes a file, or an empty folder. Returns null on success. */
    public fun delete(path: String): String? {
        val target = File(path)
        if (!target.exists()) return "$path no longer exists."
        if (target.isDirectory) {
            val children = try {
                target.list()?.size ?: 0
            } catch (error: SecurityException) {
                return "No permission to read ${target.name}."
            }
            if (children > 0) {
                // Recursive delete is the single most destructive thing a file
                // manager can do by accident, so it is not offered at all in v1.
                return "${target.name} still has $children item" +
                    (if (children == 1) "" else "s") +
                    " in it. Cope only deletes empty folders."
            }
        }
        return try {
            if (target.delete()) null else "Android refused to delete ${target.name}."
        } catch (error: SecurityException) {
            "No permission to delete ${target.name}."
        }
    }

    /** Facts for the properties dialog. Every value is measured, none is guessed. */
    public fun properties(path: String): List<Pair<String, String>> {
        val file = File(path)
        if (!file.exists()) return listOf("path" to path, "state" to "does not exist")
        val facts = ArrayList<Pair<String, String>>(9)
        facts += "name" to file.name
        facts += "folder" to (file.parent ?: "/")
        facts += "type" to if (file.isDirectory) "folder" else "file"
        if (file.isDirectory) {
            val children = try {
                file.list()?.size ?: -1
            } catch (error: SecurityException) {
                -1
            }
            facts += "items" to if (children < 0) "unreadable" else children.toString()
        } else {
            facts += "size" to "${humanSize(file.length())} (${file.length()} bytes)"
        }
        facts += "modified" to formatTime(file.lastModified())
        facts += "readable" to yesNo(file.canRead())
        facts += "writable" to yesNo(file.canWrite())
        if (file.isHidden) facts += "hidden" to "yes"
        return facts
    }

    private fun yesNo(value: Boolean): String = if (value) "yes" else "no"

    /** ISO-ish local time, no locale surprises: 2026-09-01 07:57. */
    public fun formatTime(millis: Long): String {
        if (millis <= 0L) return "unknown"
        val calendar = java.util.Calendar.getInstance()
        calendar.timeInMillis = millis
        fun pad(value: Int): String = if (value < 10) "0$value" else value.toString()
        return "${calendar.get(java.util.Calendar.YEAR)}-" +
            "${pad(calendar.get(java.util.Calendar.MONTH) + 1)}-" +
            "${pad(calendar.get(java.util.Calendar.DAY_OF_MONTH))} " +
            "${pad(calendar.get(java.util.Calendar.HOUR_OF_DAY))}:" +
            pad(calendar.get(java.util.Calendar.MINUTE))
    }

    public fun humanSize(bytes: Long): String = when {
        bytes < 0 -> "?"
        bytes < 1024 -> "$bytes B"
        bytes < 1024 * 1024 -> "${(bytes / 1024.0).format1()} KB"
        bytes < 1024L * 1024L * 1024L -> "${(bytes / (1024.0 * 1024.0)).format1()} MB"
        else -> "${(bytes / (1024.0 * 1024.0 * 1024.0)).format1()} GB"
    }

    private fun Double.format1(): String {
        val scaled = (this * 10).toLong()
        return if (scaled % 10 == 0L) "${scaled / 10}" else "${scaled / 10}.${scaled % 10}"
    }
}
