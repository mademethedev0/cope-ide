# R8 rules.
#
# THE ONE RULE THAT MATTERS: the JNI entry points.
#
# Every `external fun` in CopeNative is resolved by name at runtime from
# libcope_jni.so. R8 does not know that, so in a release build it would rename or
# strip them and the app would die with an UnsatisfiedLinkError on the first
# native call. Keeping the class and its native methods is not optional.
-keepclasseswithmembernames,includedescriptorclasses class dev.cope.ide.core.CopeNative {
    native <methods>;
}
-keep class dev.cope.ide.core.CopeNative { *; }

# The native side constructs nothing on the Java side and calls back into no
# Kotlin methods, so nothing else needs keeping for JNI.

# Compose's own consumer rules handle the runtime; these two silence warnings
# from optional code paths Compose references but this app never links.
-dontwarn org.jetbrains.annotations.**
-dontwarn kotlinx.coroutines.debug.**

# Stack traces from a release build are the only crash diagnostic this app has
# (CopeApp records the top frame). Without these, the recorded line number is
# meaningless.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
