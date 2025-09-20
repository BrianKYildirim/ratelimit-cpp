
#include <jni.h>
#include <string>
#include "ratelimit/rate_limiter.h"

using rl::RateLimiterInProc;

static RateLimiterInProc* get_singleton() {
    static RateLimiterInProc::Config cfg;
    static RateLimiterInProc* inst = new RateLimiterInProc(cfg, nullptr);
    return inst;
}

extern "C" JNIEXPORT jobject JNICALL
Java_io_ratelimit_NativeRateLimiter_allow(JNIEnv* env, jclass,
                                          jstring jkey, jint capacity, jint refillPerSec) {
    if (!jkey) return nullptr;
    const char* utf = env->GetStringUTFChars(jkey, nullptr);
    rl::Limit lim{static_cast<uint64_t>(capacity), static_cast<uint64_t>(refillPerSec)};
    rl::Decision d = get_singleton()->allow(std::string_view(utf), lim);
    env->ReleaseStringUTFChars(jkey, utf);

    jclass decisionCls = env->FindClass("io/ratelimit/NativeRateLimiter$Decision");
    if (!decisionCls) return nullptr;
    jmethodID ctor = env->GetMethodID(decisionCls, "<init>", "(ZJJ)V");
    if (!ctor) return nullptr;
    jobject obj = env->NewObject(decisionCls, ctor, (jboolean)d.allowed, (jlong)d.remaining, (jlong)d.reset_ms);
    return obj;
}
