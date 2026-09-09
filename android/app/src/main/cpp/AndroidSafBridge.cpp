#include "AndroidSafBridge.hpp"

#include <SDL_system.h>

#include <android/log.h>
#include <jni.h>

#include <cstdlib>

namespace
{
constexpr const char* LogTag = "OpenXRay";

bool ClearPendingException(JNIEnv* environment, const char* operation)
{
    if (!environment->ExceptionCheck())
        return false;

    environment->ExceptionDescribe();
    environment->ExceptionClear();
    __android_log_print(ANDROID_LOG_ERROR, LogTag, "SAF JNI exception during %s", operation);
    return true;
}

bool GetActivity(JNIEnv*& environment, jobject& activity, jclass& activityClass)
{
    environment = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    activity = reinterpret_cast<jobject>(SDL_AndroidGetActivity());
    if (!environment || !activity)
        return false;

    activityClass = environment->GetObjectClass(activity);
    if (!activityClass || ClearPendingException(environment, "activity lookup"))
    {
        environment->DeleteLocalRef(activity);
        activity = nullptr;
        return false;
    }
    return true;
}
} // namespace

bool AndroidSafListDirectory(const char* relativePath, std::vector<AndroidSafEntry>& entries)
{
    entries.clear();

    JNIEnv* environment = nullptr;
    jobject activity = nullptr;
    jclass activityClass = nullptr;
    if (!GetActivity(environment, activity, activityClass))
        return false;

    jmethodID method = environment->GetMethodID(
        activityClass, "listGameDataDirectory", "(Ljava/lang/String;)[Ljava/lang/String;");
    if (!method || ClearPendingException(environment, "directory method lookup"))
    {
        environment->DeleteLocalRef(activityClass);
        environment->DeleteLocalRef(activity);
        return false;
    }

    jstring path = environment->NewStringUTF(relativePath ? relativePath : "");
    auto result = static_cast<jobjectArray>(environment->CallObjectMethod(activity, method, path));
    environment->DeleteLocalRef(path);
    if (ClearPendingException(environment, "directory listing") || !result)
    {
        environment->DeleteLocalRef(activityClass);
        environment->DeleteLocalRef(activity);
        return false;
    }

    const jsize count = environment->GetArrayLength(result);
    if (count % 3 != 0)
    {
        __android_log_print(ANDROID_LOG_ERROR, LogTag, "Malformed SAF directory result: %d fields", count);
        environment->DeleteLocalRef(result);
        environment->DeleteLocalRef(activityClass);
        environment->DeleteLocalRef(activity);
        return false;
    }

    std::vector<std::string> fields;
    fields.reserve(static_cast<size_t>(count));
    for (jsize index = 0; index < count; ++index)
    {
        auto value = static_cast<jstring>(environment->GetObjectArrayElement(result, index));
        if (!value)
            continue;

        const char* utf = environment->GetStringUTFChars(value, nullptr);
        if (utf)
        {
            fields.emplace_back(utf);
            environment->ReleaseStringUTFChars(value, utf);
        }
        environment->DeleteLocalRef(value);
    }

    environment->DeleteLocalRef(result);
    environment->DeleteLocalRef(activityClass);
    environment->DeleteLocalRef(activity);
    if (ClearPendingException(environment, "directory result conversion"))
        return false;
    if (fields.size() != static_cast<size_t>(count))
    {
        __android_log_write(ANDROID_LOG_ERROR, LogTag, "Could not convert all SAF directory fields");
        return false;
    }

    entries.reserve(fields.size() / 3);
    for (size_t index = 0; index < fields.size(); index += 3)
    {
        AndroidSafEntry entry;
        entry.directory = fields[index] == "d";
        entry.size = std::strtoll(fields[index + 1].c_str(), nullptr, 10);
        entry.name = std::move(fields[index + 2]);
        entries.push_back(std::move(entry));
    }
    return true;
}

int AndroidSafOpenFile(const char* relativePath)
{
    JNIEnv* environment = nullptr;
    jobject activity = nullptr;
    jclass activityClass = nullptr;
    if (!GetActivity(environment, activity, activityClass))
        return -1;

    jmethodID method = environment->GetMethodID(activityClass, "openGameDataFile", "(Ljava/lang/String;)I");
    if (!method || ClearPendingException(environment, "open method lookup"))
    {
        environment->DeleteLocalRef(activityClass);
        environment->DeleteLocalRef(activity);
        return -1;
    }

    jstring path = environment->NewStringUTF(relativePath ? relativePath : "");
    const jint descriptor = environment->CallIntMethod(activity, method, path);
    environment->DeleteLocalRef(path);
    const bool failed = ClearPendingException(environment, "file open");
    environment->DeleteLocalRef(activityClass);
    environment->DeleteLocalRef(activity);
    return failed ? -1 : descriptor;
}
