
#include <jni.h>
#include <string>
#include "ratelimit/rate_limiter.h"

using rl::RateLimiterInProc;

static RateLimiterInProc* get_singleton() {
    static RateLimiterInProc::Config cfg;
    static RateLimiterInProc* inst = new RateLimiterInProc(cfg, nullptr);
    return inst;
}

// Class and constructor lookups are slow; resolve them once at load time.
static jclass g_decision_cls = nullptr;
static jmethodID g_decision_ctor = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass local = env->FindClass("io/ratelimit/NativeRateLimiter$Decision");
    if (!local) return JNI_ERR;
    g_decision_cls = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_decision_ctor = env->GetMethodID(g_decision_cls, "<init>", "(ZJJ)V");
    return g_decision_ctor ? JNI_VERSION_1_6 : JNI_ERR;
}

extern "C" JNIEXPORT jobject JNICALL
Java_io_ratelimit_NativeRateLimiter_allow(JNIEnv* env, jclass,
                                          jstring jkey, jint capacity, jint refillPerSec) {
    if (!jkey) return nullptr;
    rl::Limit lim{static_cast<uint64_t>(capacity), static_cast<uint64_t>(refillPerSec)};
    rl::Decision d;
    // Short keys are copied to the stack instead of a JVM-allocated UTF buffer.
    char buf[256];
    const jsize utf_len = env->GetStringUTFLength(jkey);
    if (utf_len <= static_cast<jsize>(sizeof buf)) {
        env->GetStringUTFRegion(jkey, 0, env->GetStringLength(jkey), buf);
        d = get_singleton()->allow(std::string_view(buf, static_cast<size_t>(utf_len)), lim);
    } else {
        const char* utf = env->GetStringUTFChars(jkey, nullptr);
        if (!utf) return nullptr;
        d = get_singleton()->allow(std::string_view(utf, static_cast<size_t>(utf_len)), lim);
        env->ReleaseStringUTFChars(jkey, utf);
    }
    return env->NewObject(g_decision_cls, g_decision_ctor, (jboolean)d.allowed, (jlong)d.remaining, (jlong)d.reset_ms);
}
