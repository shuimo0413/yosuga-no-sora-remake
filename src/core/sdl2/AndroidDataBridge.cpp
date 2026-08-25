/* SPDX-License-Identifier: MIT */
/*
 * Android JNI bridge for the external data flow:
 *  - the bootstrap activity reports the extracted data directory
 *    (nativeSetDataDir), which StorageImpl uses to resolve ./data/* to real
 *    files instead of APK assets;
 *  - nativeExtractXp3Start runs the xp3 extractor on a worker thread and
 *    reports through <outDir>.status / <outDir>.progress (the Java side
 *    polls them, mirroring the OHOS flow).
 */

#include <jni.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "xp3_extract.h"

static std::string gDataDir;

const char *AndroidDataDir_Get()
{
	return gDataDir.empty() ? nullptr : gDataDir.c_str();
}

extern "C" JNIEXPORT void JNICALL
Java_com_lightwinder_yosuganosora_hdremake_KirikiriSDL2Activity_nativeSetDataDir(
	JNIEnv *env, jobject thiz, jstring dir)
{
	if (dir == nullptr) return;
	const char *utf = env->GetStringUTFChars(dir, nullptr);
	if (utf)
	{
		gDataDir = utf;
		env->ReleaseStringUTFChars(dir, utf);
	}
}

namespace {

struct AndroidExtractCtx {
	std::string xp3Path;
	std::string outDir;
	std::string progressPath;
	std::string statusPath;
};

int AndroidProgressBridge(void *vctx, int done, int total, const char *nameUtf8)
{
	AndroidExtractCtx *ctx = static_cast<AndroidExtractCtx *>(vctx);
	if (done % 512 != 0 && done < total) return 1;
	FILE *p = fopen(ctx->progressPath.c_str(), "w");
	if (p)
	{
		fprintf(p, "%d %d %s\n", done, total, nameUtf8 ? nameUtf8 : "");
		fclose(p);
	}
	return 1;
}

void AndroidExtractWorker(AndroidExtractCtx *ctx)
{
	OHOSXp3ExtractResult res;
	memset(&res, 0, sizeof(res));
	int rc = -1;
	try
	{
		rc = OHOS_ExtractXp3(ctx->xp3Path.c_str(), ctx->outDir.c_str(),
			AndroidProgressBridge, ctx, &res);
	}
	catch (...)
	{
		snprintf(res.error, sizeof(res.error), "worker exception");
	}
	FILE *s = fopen(ctx->statusPath.c_str(), "w");
	if (s)
	{
		if (rc == 0)
		{
			fprintf(s, "ok %d %d\n", res.filesDone, res.filesTotal);
		}
		else
		{
			fprintf(s, "error %s\n", res.error);
		}
		fclose(s);
	}
	delete ctx;
}

} /* namespace */

static bool gExtractRunning = false;
static std::thread gExtractThread;

extern "C" JNIEXPORT jboolean JNICALL
Java_com_lightwinder_yosuganosora_hdremake_BootstrapActivity_nativeExtractXp3Start(
	JNIEnv *env, jobject thiz, jstring xp3Path, jstring outDir)
{
	if (gExtractRunning) return JNI_FALSE;
	if (xp3Path == nullptr || outDir == nullptr) return JNI_FALSE;
	const char *xp3 = env->GetStringUTFChars(xp3Path, nullptr);
	const char *out = env->GetStringUTFChars(outDir, nullptr);
	if (!xp3 || !out)
	{
		if (xp3) env->ReleaseStringUTFChars(xp3Path, xp3);
		if (out) env->ReleaseStringUTFChars(outDir, out);
		return JNI_FALSE;
	}
	AndroidExtractCtx *ctx = new AndroidExtractCtx();
	ctx->xp3Path = xp3;
	ctx->outDir = out;
	ctx->progressPath = ctx->outDir + ".progress";
	ctx->statusPath = ctx->outDir + ".status";
	env->ReleaseStringUTFChars(xp3Path, xp3);
	env->ReleaseStringUTFChars(outDir, out);
	FILE *p = fopen(ctx->progressPath.c_str(), "w");
	if (p) fclose(p);
	FILE *s = fopen(ctx->statusPath.c_str(), "w");
	if (s) fclose(s);
	gExtractRunning = true;
	try
	{
		gExtractThread = std::thread(AndroidExtractWorker, ctx);
	}
	catch (...)
	{
		delete ctx;
		gExtractRunning = false;
		return JNI_FALSE;
	}
	return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_lightwinder_yosuganosora_hdremake_KirikiriSDL2Activity_nativeDetachExtractThread(
	JNIEnv *env, jobject thiz)
{
	if (gExtractThread.joinable()) gExtractThread.detach();
}
