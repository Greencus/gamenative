#include <jni.h>

extern "C" JNIEXPORT jlong JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeStart(JNIEnv*, jobject) {
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeStop(JNIEnv*, jobject, jlong) {
}

extern "C" JNIEXPORT void JNICALL
Java_app_gamenative_xr_QuestVrActivity_nativeHaptic(JNIEnv*, jobject, jlong, jint, jfloat, jlong, jfloat) {
}
