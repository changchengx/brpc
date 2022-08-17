/*
 *  Copyright (c) 2022 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Author: Xu Yifeng
 */

#include "uma/systm.h"
#include "uma/mtx.h"

#define INVALID_PTHREAD 0

void mtx_init(struct mtx *m, const char *name, const char *type, int opts)
{
	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
	pthread_mutex_init(&m->mtx_lock, &attr);
	pthread_mutexattr_destroy(&attr);
	m->mtx_owner = INVALID_PTHREAD;
}

int mtx_trylock(struct mtx *m)
{
	if (pthread_mutex_trylock(&m->mtx_lock) == 0) {
		m->mtx_owner = pthread_self();
		return 1;
	}

	return 0;
}

void mtx_lock(struct mtx *m)
{
	pthread_mutex_lock(&m->mtx_lock);
	m->mtx_owner = pthread_self();
}

void mtx_unlock(struct mtx *m)
{
	m->mtx_owner = INVALID_PTHREAD;
	pthread_mutex_unlock(&m->mtx_lock);
}

void mtx_assert(struct mtx *m, int ma)
{
	if (ma & MA_OWNED) {
		KASSERT(m->mtx_owner == pthread_self(), ("mtx not owned"));
	}

	if (ma & MA_NOTOWNED) {
		KASSERT(m->mtx_owner != pthread_self(), ("mtx is owned"));
	}
}

void mtx_sleep(pthread_cond_t *cond, struct mtx *m)
{
	m->mtx_owner = INVALID_PTHREAD;
	pthread_cond_wait(cond, &m->mtx_lock);
	m->mtx_owner = pthread_self();
}

