#include <jni.h>
#include <android/log.h>
#include "carver_core.h"

#define TAG "Carver"

namespace {

std::string toStd(JNIEnv* env, jstring s) {
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL
Java_com_example_recovery_NativeCarver_carve(JNIEnv* env, jobject, jint fd, jstring outDir) {
    int n = carver::carveFd(fd, toStd(env, outDir));
    __android_log_print(ANDROID_LOG_INFO, TAG, "Recovered %d files", n);
    return n;
}

// وضع root: dd يقرأ القسم ونمسح مخرجاته مباشرة دون كتابة صورة ضخمة
JNIEXPORT jint JNICALL
Java_com_example_recovery_NativeCarver_carveRootDevice(JNIEnv* env, jobject, jstring device, jstring outDir) {
    std::string dev = toStd(env, device);
    if (!carver::safeDevicePath(dev)) return -3;
    std::string cmd = "su -c \"dd if=" + dev + " bs=4M 2>/dev/null\"";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return -4;
    int n = carver::carveFd(fileno(pipe), toStd(env, outDir));
    pclose(pipe);
    return n;
}

JNIEXPORT jlong JNICALL
Java_com_example_recovery_NativeCarver_progressBytes(JNIEnv*, jobject) { return carver::gBytes.load(); }

JNIEXPORT jint JNICALL
Java_com_example_recovery_NativeCarver_progressCount(JNIEnv*, jobject) { return carver::gCount.load(); }

JNIEXPORT void JNICALL
Java_com_example_recovery_NativeCarver_cancel(JNIEnv*, jobject) { carver::gCancel = true; }

}
