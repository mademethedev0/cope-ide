// Fuzzy matching for the command palette, and path normalisation for relative
// markdown links.
//
// Deliberately a plain Kotlin file with no Compose imports: both functions are
// pure, both are covered by JVM unit tests, and a test that has to load a
// Compose-compiled class to call one function is a test that breaks for reasons
// unrelated to what it checks.
package dev.cope.ide.ui

/**
 * Subsequence match, scored. Returns 0 for "no match" and a positive score
 * otherwise, higher being better.
 *
 * Scoring, in order of weight:
 *   * a hit right after a word boundary (space, `/`, `_`, `.`, `-`) is worth most,
 *     so "gtl" finds "Go to line" rather than "Toggle indent guides";
 *   * a hit immediately after the previous one is worth nearly as much, so a
 *     literal substring beats a scattered subsequence;
 *   * shorter candidates win ties, so "Save" beats "Save as…" for "save".
 *
 * An empty query matches everything with score 1, which keeps the initial list in
 * its natural order instead of shuffling it.
 */
public fun fuzzyScore(candidate: String, query: String): Int {
    if (query.isEmpty()) return 1
    var score = 0
    var at = 0
    var previousMatch = -2
    for (needle in query) {
        if (needle == ' ') continue
        val lower = needle.lowercaseChar()
        var found = -1
        var i = at
        while (i < candidate.length) {
            if (candidate[i].lowercaseChar() == lower) {
                found = i
                break
            }
            i++
        }
        if (found < 0) return 0
        score += 1
        if (found == previousMatch + 1) score += 3
        if (found == 0 || isBoundary(candidate[found - 1])) score += 4
        previousMatch = found
        at = found + 1
    }
    return score * 100 - candidate.length
}

private fun isBoundary(c: Char): Boolean =
    c == ' ' || c == '/' || c == '_' || c == '.' || c == '-'

/**
 * Collapses `.` and `..` segments so a relative markdown link resolves to a real
 * absolute path. A `..` at the root is dropped rather than escaping it, which is
 * what every path resolver does and what stops `../../../etc` from meaning
 * anything.
 */
public fun normalisePath(path: String): String {
    val parts = ArrayList<String>(8)
    for (segment in path.split('/')) {
        when (segment) {
            "", "." -> Unit
            ".." -> if (parts.isNotEmpty()) parts.removeAt(parts.size - 1)
            else -> parts += segment
        }
    }
    return "/" + parts.joinToString("/")
}

/** "a/b/c/d" -> "…/c/d", so a long path never crowds out the file name. */
public fun shortenTail(path: String, keep: Int = 2): String {
    val parts = path.trim('/').split('/').filter { it.isNotEmpty() }
    if (parts.size <= keep) return path
    return "…/" + parts.takeLast(keep).joinToString("/")
}
