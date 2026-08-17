/* SPDX-License-Identifier: MIT */

/*
 * OpenHarmony (musl libc) compatibility shims for the Kirikiri engine.
 *
 * The OpenHarmony musl declares pthread_setcanceltype in <pthread.h> but
 * does not export an implementation. The engine only uses it as a
 * cancellation hint; returning success with the deferred default is safe.
 */

#include <pthread.h>

int pthread_setcanceltype(int type, int *oldtype)
{
    (void)type;
    if (oldtype != NULL)
    {
        *oldtype = PTHREAD_CANCEL_DEFERRED;
    }
    return 0;
}

