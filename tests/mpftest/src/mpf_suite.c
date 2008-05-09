/*
 * Copyright 2008 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <apr_thread_cond.h>
#include "apt_test_suite.h"
#include "mpf_engine.h"
#include "mpf_user.h"
#include "mpf_audio_file_descriptor.h"
#include "mpf_rtp_descriptor.h"
#include "apt_consumer_task.h"
#include "apt_log.h"

typedef struct mpf_suite_session_t mpf_suite_session_t;

struct mpf_suite_session_t {
	apr_pool_t        *pool;

	mpf_context_t     *context;
	mpf_termination_t *termination1;
	mpf_termination_t *termination2;
};

typedef struct mpf_suite_engine_t mpf_suite_engine_t;

struct mpf_suite_engine_t {
	apt_consumer_task_t *consumer_task;
	apt_task_t          *engine_task;

	apr_thread_cond_t   *wait_object;
	apr_thread_mutex_t  *wait_object_mutex;
};

static mpf_audio_file_descriptor_t* mpf_file_reader_descriptor_create(mpf_suite_session_t *session)
{
	mpf_codec_descriptor_t *codec_descriptor;
	mpf_audio_file_descriptor_t *descriptor = apr_palloc(session->pool,sizeof(mpf_audio_file_descriptor_t));
	descriptor->mask = FILE_READER;
	descriptor->read_handle = fopen("demo.pcm","rb");
	descriptor->write_handle = NULL;

	codec_descriptor = &descriptor->codec_descriptor;
	codec_descriptor->payload_type = 11;
	codec_descriptor->name = "L16";
	codec_descriptor->sampling_rate = 8000;
	codec_descriptor->channel_count = 1;
	return descriptor;
}

static mpf_audio_file_descriptor_t* mpf_file_writer_descriptor_create(mpf_suite_session_t *session)
{
	mpf_codec_descriptor_t *codec_descriptor;
	mpf_audio_file_descriptor_t *descriptor = apr_palloc(session->pool,sizeof(mpf_audio_file_descriptor_t));
	descriptor->mask = FILE_WRITER;
	descriptor->write_handle = fopen("demo_out.pcm","wb");
	descriptor->read_handle = NULL;

	codec_descriptor = &descriptor->codec_descriptor;
	codec_descriptor->payload_type = 11;
	codec_descriptor->name = "L16";
	codec_descriptor->sampling_rate = 8000;
	codec_descriptor->channel_count = 1;
	return descriptor;
}

static mpf_rtp_stream_descriptor_t* mpf_rtp_local_descriptor_create(mpf_suite_session_t *session)
{
	mpf_rtp_stream_descriptor_t *descriptor = apr_palloc(session->pool,sizeof(mpf_rtp_stream_descriptor_t));
	mpf_rtp_stream_descriptor_init(descriptor);
	descriptor->mode = STREAM_MODE_NONE;
	descriptor->mask = RTP_MEDIA_DESCRIPTOR_LOCAL;
	descriptor->local.ip = "127.0.0.1";
	descriptor->local.port = 5000;
	return descriptor;
}

static mpf_rtp_stream_descriptor_t* mpf_rtp_remote_descriptor_create(mpf_suite_session_t *session)
{
	mpf_codec_list_t *codec_list;
	mpf_codec_descriptor_t *codec_descriptor;
	mpf_rtp_stream_descriptor_t *descriptor = apr_palloc(session->pool,sizeof(mpf_rtp_stream_descriptor_t));
	mpf_rtp_stream_descriptor_init(descriptor);
	descriptor->mode = STREAM_MODE_SEND_RECEIVE;
	descriptor->mask = RTP_MEDIA_DESCRIPTOR_REMOTE;
	descriptor->remote.ip = "127.0.0.1";
	descriptor->remote.port = 5002;
	codec_list = &descriptor->remote.codec_list;
	mpf_codec_list_init(codec_list,2,session->pool);
	codec_descriptor = mpf_codec_list_add(codec_list);
	if(codec_descriptor) {
		codec_descriptor->payload_type = 0;
	}
	codec_descriptor = mpf_codec_list_add(codec_list);
	if(codec_descriptor) {
		codec_descriptor->payload_type = 96;
		codec_descriptor->name = "PCMU";
		codec_descriptor->sampling_rate = 16000;
		codec_descriptor->channel_count = 1;
	}

	return descriptor;
}

static void task_on_start_complete(apt_task_t *task)
{
	mpf_suite_session_t *session;
	apt_task_t *consumer_task;
	mpf_suite_engine_t *suite_engine;
	apt_task_msg_t *msg;
	mpf_message_t *mpf_message;
	apr_pool_t *pool = NULL;

	consumer_task = apt_task_object_get(task);
	suite_engine = apt_task_object_get(consumer_task);

	apt_log(APT_PRIO_INFO,"On Task Start");
	apr_pool_create(&pool,NULL);
	session = apr_palloc(pool,sizeof(mpf_suite_session_t));
	session->pool = pool;
	session->context = NULL;
	session->termination1 = NULL;
	session->termination2 = NULL;

	apt_log(APT_PRIO_INFO,"Create MPF Context");
	session->context = mpf_context_create(session,2,pool);

	apt_log(APT_PRIO_INFO,"Create Termination [1]");
	session->termination1 = mpf_file_termination_create(session,session->pool);

	apt_log(APT_PRIO_INFO,"Add Termination [1] to Context");
	msg = apt_task_msg_get(task);
	msg->type = TASK_MSG_USER;
	mpf_message = (mpf_message_t*) msg->data;

	mpf_message->message_type = MPF_MESSAGE_TYPE_REQUEST;
	mpf_message->command_id = MPF_COMMAND_ADD;
	mpf_message->context = session->context;
	mpf_message->termination = session->termination1;
	mpf_message->descriptor = mpf_file_reader_descriptor_create(session);
	apt_task_msg_signal(suite_engine->engine_task,msg);

	apt_log(APT_PRIO_INFO,"Create Termination [2]");
//	session->termination2 = mpf_file_termination_create(session,session->pool);
	session->termination2 = mpf_rtp_termination_create(session,session->pool);

	apt_log(APT_PRIO_INFO,"Add Termination [2] to Context");
	msg = apt_task_msg_get(task);
	msg->type = TASK_MSG_USER;
	mpf_message = (mpf_message_t*) msg->data;

	mpf_message->message_type = MPF_MESSAGE_TYPE_REQUEST;
	mpf_message->command_id = MPF_COMMAND_ADD;
	mpf_message->context = session->context;
	mpf_message->termination = session->termination2;
//	mpf_message->descriptor = mpf_file_writer_descriptor_create(session);
	mpf_message->descriptor = mpf_rtp_local_descriptor_create(session);
	apt_task_msg_signal(suite_engine->engine_task,msg);
}

static void task_on_terminate_complete(apt_task_t *task)
{
	apt_log(APT_PRIO_INFO,"On Task Terminate");
}

static apt_bool_t task_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
	const mpf_message_t *mpf_message = (const mpf_message_t*) msg->data;
	if(mpf_message->message_type == MPF_MESSAGE_TYPE_RESPONSE) {
		apt_log(APT_PRIO_DEBUG,"Process MPF Response");
		if(mpf_message->command_id == MPF_COMMAND_ADD) {
			apt_log(APT_PRIO_DEBUG,"On Add Termination");
			if(mpf_message->termination) {
				mpf_suite_session_t *session;
				session = mpf_termination_object_get(mpf_message->termination);
				if(session->termination2 == mpf_message->termination) {
					apt_task_msg_t *msg;
					mpf_message_t *request;
					apt_task_t *consumer_task;
					mpf_suite_engine_t *suite_engine;

					consumer_task = apt_task_object_get(task);
					suite_engine = apt_task_object_get(consumer_task);

					msg = apt_task_msg_get(task);
					msg->type = TASK_MSG_USER;
					request = (mpf_message_t*) msg->data;

					request->message_type = MPF_MESSAGE_TYPE_REQUEST;
					request->command_id = MPF_COMMAND_MODIFY;
					request->context = session->context;
					request->termination = session->termination2;
					request->descriptor = mpf_rtp_remote_descriptor_create(session);
					apt_task_msg_signal(suite_engine->engine_task,msg);
				}
			}
		}
		else if(mpf_message->command_id == MPF_COMMAND_SUBTRACT) {
			apt_log(APT_PRIO_DEBUG,"On Subtract Termination");
			if(mpf_message->termination) {
				mpf_suite_session_t *session;
				session = mpf_termination_object_get(mpf_message->termination);
				if(session->termination1 == mpf_message->termination) {
					session->termination1 = NULL;
				}
				if(session->termination2 == mpf_message->termination) {
					session->termination2 = NULL;
				}
				mpf_termination_destroy(mpf_message->termination);

				if(!session->termination1 && !session->termination2) {
					apt_task_t *consumer_task;
					mpf_suite_engine_t *suite_engine;

					mpf_context_destroy(session->context);
					session->context = NULL;
					apr_pool_destroy(session->pool);

					consumer_task = apt_task_object_get(task);
					suite_engine = apt_task_object_get(consumer_task);

					apr_thread_mutex_lock(suite_engine->wait_object_mutex);
					apr_thread_cond_signal(suite_engine->wait_object);
					apr_thread_mutex_unlock(suite_engine->wait_object_mutex);
				}
			}
		}
	}
	else if(mpf_message->message_type == MPF_MESSAGE_TYPE_EVENT) {
		apt_task_t *consumer_task;
		mpf_suite_engine_t *suite_engine;
		apt_task_msg_t *msg;
		mpf_message_t *request;
		mpf_suite_session_t *session;
		apt_log(APT_PRIO_DEBUG,"Process MPF Event");
		if(mpf_message->termination) {
			session = mpf_termination_object_get(mpf_message->termination);
			if(session->termination1) {
				apt_log(APT_PRIO_INFO,"Subtract Termination [1] from Context");
				msg = apt_task_msg_get(task);
				msg->type = TASK_MSG_USER;
				request = (mpf_message_t*) msg->data;

				request->message_type = MPF_MESSAGE_TYPE_REQUEST;
				request->command_id = MPF_COMMAND_SUBTRACT;
				request->context = session->context;
				request->termination = session->termination1;

				consumer_task = apt_task_object_get(task);
				suite_engine = apt_task_object_get(consumer_task);
				apt_task_msg_signal(suite_engine->engine_task,msg);
			}
			if(session->termination2) {
				apt_log(APT_PRIO_INFO,"Subtract Termination [2] from Context");
				msg = apt_task_msg_get(task);
				msg->type = TASK_MSG_USER;
				request = (mpf_message_t*) msg->data;

				request->message_type = MPF_MESSAGE_TYPE_REQUEST;
				request->command_id = MPF_COMMAND_SUBTRACT;
				request->context = session->context;
				request->termination = session->termination2;

				consumer_task = apt_task_object_get(task);
				suite_engine = apt_task_object_get(consumer_task);
				apt_task_msg_signal(suite_engine->engine_task,msg);
			}
		}
	}

	return TRUE;
}

static apt_bool_t mpf_test_run(apt_test_suite_t *suite, int argc, const char * const *argv)
{
	mpf_suite_engine_t *suite_engine;
	mpf_engine_t *engine;

	apt_task_t *task;
	apt_task_vtable_t vtable;
	apt_task_msg_pool_t *msg_pool;

	suite_engine = apr_palloc(suite->pool,sizeof(mpf_suite_engine_t));

	engine = mpf_engine_create(suite->pool);
	if(!engine) {
		apt_log(APT_PRIO_WARNING,"Failed to Create MPF Engine");
		return FALSE;
	}
	suite_engine->engine_task = mpf_task_get(engine);

	apt_task_vtable_reset(&vtable);
	vtable.process_msg = task_msg_process;
	vtable.on_start_complete = task_on_start_complete;
	vtable.on_terminate_complete = task_on_terminate_complete;

	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(mpf_message_t),suite->pool);

	apt_log(APT_PRIO_NOTICE,"Create Consumer Task");
	suite_engine->consumer_task = apt_consumer_task_create(suite_engine,&vtable,msg_pool,suite->pool);
	if(!suite_engine->consumer_task) {
		apt_log(APT_PRIO_WARNING,"Failed to Create Consumer Task");
		return FALSE;
	}
	task = apt_consumer_task_base_get(suite_engine->consumer_task);

	apt_task_add(task,suite_engine->engine_task);

	apr_thread_mutex_create(&suite_engine->wait_object_mutex,APR_THREAD_MUTEX_UNNESTED,suite->pool);
	apr_thread_cond_create(&suite_engine->wait_object,suite->pool);

	apt_log(APT_PRIO_INFO,"Start Task");
	if(apt_task_start(task) == FALSE) {
		apt_log(APT_PRIO_WARNING,"Failed to Start Task");
		apt_task_destroy(task);
		return FALSE;
	}

	apr_thread_mutex_lock(suite_engine->wait_object_mutex);
	apr_thread_cond_wait(suite_engine->wait_object,suite_engine->wait_object_mutex);
	apr_thread_mutex_unlock(suite_engine->wait_object_mutex);
	
	apt_log(APT_PRIO_INFO,"Terminate Task [wait till complete]");
	apt_task_terminate(task,TRUE);
	apt_log(APT_PRIO_NOTICE,"Destroy Task");
	apt_task_destroy(task);

	apr_thread_cond_destroy(suite_engine->wait_object);
	apr_thread_mutex_destroy(suite_engine->wait_object_mutex);
	return TRUE;
}

apt_test_suite_t* mpf_suite_create(apr_pool_t *pool)
{
	apt_test_suite_t *suite = apt_test_suite_create(pool,"mpf",NULL,mpf_test_run);
	return suite;
}
