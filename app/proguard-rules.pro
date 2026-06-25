# Keep NCNN JNI bridge and native-referenced classes.
-keep class com.aimassistant.NativeBridge { *; }

# Keep parcelable Intent data for MediaProjection.
-keep class android.content.Intent { *; }
