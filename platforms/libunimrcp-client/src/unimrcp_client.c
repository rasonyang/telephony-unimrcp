/*
 * Copyright 2008-2010 Arsen Chaloyan
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
 *
 * $Id$
 */

#include <stdlib.h>
#include <apr_xml.h>
#include <apr_fnmatch.h>
#include <apr_version.h>
#include "uni_version.h"
#include "unimrcp_client.h"
#include "mrcp_resource_loader.h"
#include "mpf_engine.h"
#include "mpf_codec_manager.h"
#include "mpf_rtp_termination_factory.h"
#include "mrcp_sofiasip_client_agent.h"
#include "mrcp_unirtsp_client_agent.h"
#include "mrcp_client_connection.h"
#include "apt_net.h"
#include "apt_log.h"

#define CONF_FILE_NAME            "unimrcpclient.xml"
#define DEFAULT_CONF_DIR_PATH     "../conf"

#define DEFAULT_IP_ADDRESS        "127.0.0.1"
#define DEFAULT_SIP_PORT          8062
#define DEFAULT_RTP_PORT_MIN      4000
#define DEFAULT_RTP_PORT_MAX      5000

#define DEFAULT_SOFIASIP_UA_NAME  "UniMRCP SofiaSIP"
#define DEFAULT_SDP_ORIGIN        "UniMRCPClient"
#define DEFAULT_RESOURCE_LOCATION "media"

#define XML_FILE_BUFFER_LENGTH    16000

/** UniMRCP client loader */
typedef struct unimrcp_client_loader_t unimrcp_client_loader_t;

/** UniMRCP client loader */
struct unimrcp_client_loader_t {
	/** MRCP client */
	mrcp_client_t *client;
	/** XML document */
	apr_xml_doc   *doc;
	/** Pool to allocate memory from */
	apr_pool_t    *pool;

	/** Default ip address (named property) */
	const char    *ip;
	/** Default external (NAT) ip address (named property) */
	const char    *ext_ip;
	/** Default server ip address (named property) */
	const char    *server_ip;
	
	/** Implicitly detected, cached ip address */
	const char    *auto_ip;
};

static apt_bool_t unimrcp_client_load(unimrcp_client_loader_t *loader, const char *dir_path, const char *file_name);
static apt_bool_t unimrcp_client_load2(unimrcp_client_loader_t *loader, const char *xmlconfig);

/** Initialize client -- common to unimrcp_client_create and unimrcp_client_create2 */
static unimrcp_client_loader_t* unimrcp_client_init(apt_dir_layout_t *dir_layout)
{
	apr_pool_t *pool;
	mrcp_client_t *client;
	unimrcp_client_loader_t *loader;

	apt_log(APT_LOG_MARK,APT_PRIO_NOTICE,"UniMRCP Client ["UNI_VERSION_STRING"]");
	apt_log(APT_LOG_MARK,APT_PRIO_INFO,"APR ["APR_VERSION_STRING"]");
	client = mrcp_client_create(dir_layout);
	if(!client) {
		return NULL;
	}
	pool = mrcp_client_memory_pool_get(client);
	if(!pool) {
		return NULL;
	}

	loader = apr_palloc(pool,sizeof(unimrcp_client_loader_t));
	loader->doc = NULL;
	loader->client = client;
	loader->pool = pool;
	loader->ip = NULL;
	loader->ext_ip = NULL;
	loader->server_ip = NULL;
	loader->auto_ip = NULL;
	return loader;
}

/** Create and load UniMRCP client from directory layout */
MRCP_DECLARE(mrcp_client_t*) unimrcp_client_create(apt_dir_layout_t *dir_layout)
{
	const char *dir_path;
	unimrcp_client_loader_t *loader;

	if(!dir_layout) {
		return NULL;
	}

	loader = unimrcp_client_init(dir_layout);
	if (!loader)
		return NULL;

	dir_path = dir_layout->conf_dir_path;
	if(!dir_path) {
		dir_path = DEFAULT_CONF_DIR_PATH;
	}

	if(unimrcp_client_load(loader,dir_path,CONF_FILE_NAME) == FALSE) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Failed to Load UniMRCP Client Document");
	}

	return loader->client;
}

/** Create UniMRCP client from XML string configuration */
MRCP_DECLARE(mrcp_client_t*) unimrcp_client_create2(const char *xmlconfig)
{
	unimrcp_client_loader_t *loader;

	loader = unimrcp_client_init(NULL);
	if (!loader)
		return NULL;

	if(unimrcp_client_load2(loader,xmlconfig) == FALSE) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Failed to Process UniMRCP Client Configuration");
	}

	return loader->client;
}

/** Check whether specified attribute is valid */
static APR_INLINE apt_bool_t is_attr_valid(const apr_xml_attr *attr)
{
	return (attr && attr->value && attr->value != '\0');
}

/** Check whether specified attribute is enabled (true) */
static APR_INLINE apt_bool_t is_attr_enabled(const apr_xml_attr *attr)
{
	if(attr && strcasecmp(attr->value,"false") == 0) {
		return FALSE;
	}
	return TRUE;
}

/** Check whether cdata is valid */
static APR_INLINE apt_bool_t is_cdata_valid(const apr_xml_elem *elem)
{
	return (elem->first_cdata.first && elem->first_cdata.first->text);
}

/** Get text cdata */
static APR_INLINE const char* cdata_text_get(const apr_xml_elem *elem)
{
	return elem->first_cdata.first->text;
}

/** Get boolean cdata */
static APR_INLINE apt_bool_t cdata_bool_get(const apr_xml_elem *elem)
{
	return (strcasecmp(elem->first_cdata.first->text,"true") == 0) ? TRUE : FALSE;
}

/** Copy cdata */
static APR_INLINE char* cdata_copy(const apr_xml_elem *elem, apr_pool_t *pool)
{
	return apr_pstrdup(pool,elem->first_cdata.first->text);
}

/** Get generic "id" and "enable" attributes */
static apt_bool_t header_attribs_get(const apr_xml_elem *elem, const apr_xml_attr **id, const apr_xml_attr **enable)
{
	const apr_xml_attr *attr;
	if(!id || !enable) {
		return FALSE;
	}

	*id = NULL;
	*enable = NULL;
	for(attr = elem->attr; attr; attr = attr->next) {
		if(strcasecmp(attr->name,"id") == 0) {
			*id = attr;
		}
		else if(strcasecmp(attr->name,"enable") == 0) {
			*enable = attr;
		}
	}
	
	if(is_attr_valid(*id) == FALSE) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Missing Required Attribute <id> in Element <%s>",elem->name);
		return FALSE;
	}
	return TRUE;
}

/** Get generic "name" and "value" attributes */
static apt_bool_t name_value_attribs_get(const apr_xml_elem *elem, const apr_xml_attr **name, const apr_xml_attr **value)
{
	const apr_xml_attr *attr;
	if(!name || !value) {
		return FALSE;
	}

	*name = NULL;
	*value = NULL;
	for(attr = elem->attr; attr; attr = attr->next) {
		if(strcasecmp(attr->name,"name") == 0) {
			*name = attr;
		}
		else if(strcasecmp(attr->name,"value") == 0) {
			*value = attr;
		}
	}
	return (*name && *value) ? TRUE : FALSE;
}

static char* unimrcp_client_ip_address_get(unimrcp_client_loader_t *loader, const apr_xml_elem *elem, const char *default_ip)
{
	const apr_xml_attr *attr = NULL;
	for(attr = elem->attr; attr; attr = attr->next) {
		if(strcasecmp(attr->name,"type") == 0) {
			break;
		}
	}

	if(attr && strcasecmp(attr->value,"auto") == 0) {
		/* implicitly detect ip address, if not already detected */
		if(!loader->auto_ip) {
			char *auto_addr = DEFAULT_IP_ADDRESS;
			apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Detecting IP Address");
			apt_ip_get(&auto_addr,loader->pool);
			loader->auto_ip = auto_addr;
		}
		return apr_pstrdup(loader->pool,loader->auto_ip);
	}

	if(is_cdata_valid(elem)) {
		/* use specified ip address */
		return cdata_copy(elem,loader->pool);
	}

	/* use default ip address */
	return apr_pstrdup(loader->pool,loader->ip);
}



/** Load resource */
static apt_bool_t unimrcp_client_resource_load(mrcp_resource_loader_t *resource_loader, const apr_xml_elem *root, apr_pool_t *pool)
{
	apt_str_t resource_class;
	const apr_xml_attr *id_attr;
	const apr_xml_attr *enable_attr;
	apt_string_reset(&resource_class);

	if(header_attribs_get(root,&id_attr,&enable_attr) == FALSE) {
		return FALSE;
	}
	
	if(is_attr_enabled(enable_attr) == FALSE) {
		return TRUE;
	}

	apt_string_set(&resource_class,id_attr->value);
	return mrcp_resource_load(resource_loader,&resource_class);
}

/** Load resource factory */
static apt_bool_t unimrcp_client_resource_factory_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;
	mrcp_resource_factory_t *resource_factory;
	mrcp_resource_loader_t *resource_loader = mrcp_resource_loader_create(FALSE,loader->pool);
	if(!resource_loader) {
		return FALSE;
	}

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Resources");
	for(elem = root->first_child; elem; elem = elem->next) {
		if(strcasecmp(elem->name,"resource") == 0) {
			unimrcp_client_resource_load(resource_loader,elem,loader->pool);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	
	resource_factory = mrcp_resource_factory_get(resource_loader);
	return mrcp_client_resource_factory_register(loader->client,resource_factory);
}

/** Load SofiaSIP signaling agent */
static apt_bool_t unimrcp_client_sip_uac_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_sig_agent_t *agent;
	mrcp_sofia_client_config_t *config;

	config = mrcp_sofiasip_client_config_alloc(loader->pool);
	config->local_port = DEFAULT_SIP_PORT;
	config->user_agent_name = DEFAULT_SOFIASIP_UA_NAME;
	config->origin = DEFAULT_SDP_ORIGIN;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading SofiaSIP Agent <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"sip-ip") == 0) {
			config->local_ip = unimrcp_client_ip_address_get(loader,elem,loader->ip);
		}
		else if(strcasecmp(elem->name,"sip-ext-ip") == 0) {
			config->ext_ip = unimrcp_client_ip_address_get(loader,elem,loader->ext_ip);
		}
		else if(strcasecmp(elem->name,"sip-port") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->local_port = (apr_port_t)atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"sip-transport") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->transport = cdata_copy(elem,loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"ua-name") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->user_agent_name = cdata_copy(elem,loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"sdp-origin") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->origin = cdata_copy(elem,loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"sip-t1") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->sip_t1 = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"sip-t2") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->sip_t2 = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"sip-t4") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->sip_t4 = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"sip-t1x64") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->sip_t1x64 = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"sip-message-output") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->tport_log = cdata_bool_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"sip-message-dump") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->tport_dump_file = cdata_copy(elem,loader->pool);
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	if(!config->local_ip) {
		/* use default ip address if not specified */
		config->local_ip = apr_pstrdup(loader->pool,loader->ip);
	}
	if(!config->ext_ip && loader->ext_ip) {
		/* use default ext ip address if not specified */
		config->ext_ip = apr_pstrdup(loader->pool,loader->ext_ip);
	}

	agent = mrcp_sofiasip_client_agent_create(id,config,loader->pool);
	return mrcp_client_signaling_agent_register(loader->client,agent);
}

/** Load UniRTSP signaling agent */
static apt_bool_t unimrcp_client_rtsp_uac_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_sig_agent_t *agent;
	rtsp_client_config_t *config;

	config = mrcp_unirtsp_client_config_alloc(loader->pool);
	config->origin = DEFAULT_SDP_ORIGIN;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading UniRTSP Agent <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"sdp-origin") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->origin = cdata_copy(elem,loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"max-connection-count") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->max_connection_count = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"request-timeout") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				config->request_timeout = atol(cdata_text_get(elem));
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	
	agent = mrcp_unirtsp_client_agent_create(id,config,loader->pool);
	return mrcp_client_signaling_agent_register(loader->client,agent);
}

/** Load MRCPv2 connection agent */
static apt_bool_t unimrcp_client_mrcpv2_uac_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_connection_agent_t *agent;
	apr_size_t max_connection_count = 100;
	apt_bool_t offer_new_connection = FALSE;
	const char *rx_buffer_size = NULL;
	const char *tx_buffer_size = NULL;
	const char *request_timeout = NULL;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading MRCPv2 Agent <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"max-connection-count") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				max_connection_count = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"offer-new-connection") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				offer_new_connection = cdata_bool_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"rx-buffer-size") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rx_buffer_size = cdata_text_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"tx-buffer-size") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				tx_buffer_size = cdata_text_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"request-timeout") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				request_timeout = cdata_text_get(elem);
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	agent = mrcp_client_connection_agent_create(id,max_connection_count,offer_new_connection,loader->pool);
	if(agent) {
		if(rx_buffer_size) {
			mrcp_client_connection_rx_size_set(agent,atol(rx_buffer_size));
		}
		if(tx_buffer_size) {
			mrcp_client_connection_tx_size_set(agent,atol(tx_buffer_size));
		}
		if(request_timeout) {
			mrcp_client_connection_timeout_set(agent,atol(request_timeout));
		}
	}
	return mrcp_client_connection_agent_register(loader->client,agent);
}

/** Load media engine */
static apt_bool_t unimrcp_client_media_engine_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mpf_engine_t *media_engine;
	unsigned long realtime_rate = 1;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Media Engine <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"realtime-rate") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				realtime_rate = atol(cdata_text_get(elem));
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	
	media_engine = mpf_engine_create(id,loader->pool);
	if(media_engine) {
		mpf_engine_scheduler_rate_set(media_engine,realtime_rate);
	}
	return mrcp_client_media_engine_register(loader->client,media_engine);
}

/** Load RTP factory */
static apt_bool_t unimrcp_client_rtp_factory_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	char *rtp_ip = NULL;
	char *rtp_ext_ip = NULL;
	mpf_termination_factory_t *rtp_factory;
	mpf_rtp_config_t *rtp_config;

	rtp_config = mpf_rtp_config_alloc(loader->pool);
	rtp_config->rtp_port_min = DEFAULT_RTP_PORT_MIN;
	rtp_config->rtp_port_max = DEFAULT_RTP_PORT_MAX;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading RTP Factory <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"rtp-ip") == 0) {
			rtp_ip = unimrcp_client_ip_address_get(loader,elem,loader->ip);
		}
		else if(strcasecmp(elem->name,"rtp-ext-ip") == 0) {
			rtp_ext_ip = unimrcp_client_ip_address_get(loader,elem,loader->ext_ip);
		}
		else if(strcasecmp(elem->name,"rtp-port-min") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtp_config->rtp_port_min = (apr_port_t)atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"rtp-port-max") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtp_config->rtp_port_max = (apr_port_t)atol(cdata_text_get(elem));
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}    
	
	if(rtp_ip) {
		apt_string_set(&rtp_config->ip,rtp_ip);
	}
	else {
		apt_string_set(&rtp_config->ip,loader->ip);
	}
	if(rtp_ext_ip) {
		apt_string_set(&rtp_config->ext_ip,rtp_ext_ip);
	}
	else if(loader->ext_ip){
		apt_string_set(&rtp_config->ext_ip,loader->ext_ip);
	}

	rtp_factory = mpf_rtp_termination_factory_create(rtp_config,loader->pool);
	return mrcp_client_rtp_factory_register(loader->client,rtp_factory,id);
}


/** Load SIP settings */
static apt_bool_t unimrcp_client_sip_settings_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_sig_settings_t *settings = mrcp_signaling_settings_alloc(loader->pool);

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading SIP Settings <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"server-ip") == 0) {
			settings->server_ip = unimrcp_client_ip_address_get(loader,elem,loader->server_ip);
		}
		else if(strcasecmp(elem->name,"server-port") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->server_port = (apr_port_t)atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"server-username") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->user_name = cdata_copy(elem,loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"force-destination") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->force_destination = cdata_bool_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"feature-tags") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->feature_tags = cdata_copy(elem,loader->pool);
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	
	if(!settings->server_ip) {
		settings->server_ip = apr_pstrdup(loader->pool,loader->server_ip);
	}
	apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Create SIP Settings %s:%hu",settings->server_ip,settings->server_port);
	return mrcp_client_signaling_settings_register(loader->client,settings,id);
}

/** Load RTSP settings */
static apt_bool_t unimrcp_client_rtsp_settings_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_sig_settings_t *settings = mrcp_signaling_settings_alloc(loader->pool);
	settings->resource_location = DEFAULT_RESOURCE_LOCATION;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading RTSP Settings <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"server-ip") == 0) {
			settings->server_ip = unimrcp_client_ip_address_get(loader,elem,loader->server_ip);
		}
		else if(strcasecmp(elem->name,"server-port") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->server_port = (apr_port_t)atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"force-destination") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->force_destination = cdata_bool_get(elem);
			}
		}
		else if(strcasecmp(elem->name,"resource-location") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				settings->resource_location = cdata_copy(elem,loader->pool);
			}
			else {
				settings->resource_location = "";
			}
		}
		else if(strcasecmp(elem->name,"resource-map") == 0) {
			const apr_xml_attr *name_attr;
			const apr_xml_attr *value_attr;
			const apr_xml_elem *child_elem;
			apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Resource Map");
			for(child_elem = elem->first_child; child_elem; child_elem = child_elem->next) {
				if(name_value_attribs_get(child_elem,&name_attr,&value_attr) == TRUE) {
					apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Param %s:%s",name_attr->value,value_attr->value);
					apr_table_set(settings->resource_map,name_attr->value,value_attr->value);
				}
			}    
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	
	if(!settings->server_ip) {
		settings->server_ip = apr_pstrdup(loader->pool,loader->server_ip);
	}
	apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Create RTSP Settings %s:%hu",settings->server_ip,settings->server_port);
	return mrcp_client_signaling_settings_register(loader->client,settings,id);
}

/** Load jitter buffer settings */
static apt_bool_t unimrcp_client_jb_settings_load(unimrcp_client_loader_t *loader, mpf_jb_config_t *jb, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Jitter Buffer Settings");
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"playout-delay") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				jb->initial_playout_delay = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"min-playout-delay") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				jb->min_playout_delay = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"max-playout-delay") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				jb->max_playout_delay = atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"adaptive") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				jb->adaptive = (apr_byte_t) atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"time-skew-detection") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				jb->time_skew_detection = (apr_byte_t) atol(cdata_text_get(elem));
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	return TRUE;
}

/** Load RTCP settings */
static apt_bool_t unimrcp_client_rtcp_settings_load(unimrcp_client_loader_t *loader, mpf_rtp_settings_t *rtcp_settings, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;
	const apr_xml_attr *attr = NULL;
	for(attr = root->attr; attr; attr = attr->next) {
		if(strcasecmp(attr->name,"enable") == 0) {
			break;
		}
	}

	if(is_attr_enabled(attr) == FALSE) {
		/* RTCP is disabled, skip the rest */
		return TRUE;
	}

	rtcp_settings->rtcp = TRUE;
	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading RTCP Settings");
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"rtcp-bye") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtcp_settings->rtcp_bye_policy = atoi(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"tx-interval") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtcp_settings->rtcp_tx_interval = (apr_uint16_t)atoi(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"rx-resolution") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtcp_settings->rtcp_rx_resolution = (apr_uint16_t)atol(cdata_text_get(elem));
			}
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}
	return TRUE;
}

/** Load RTP settings */
static apt_bool_t unimrcp_client_rtp_settings_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mpf_rtp_settings_t *rtp_settings = mpf_rtp_settings_alloc(loader->pool);

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading RTP Settings <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"jitter-buffer") == 0) {
			unimrcp_client_jb_settings_load(loader,&rtp_settings->jb_config,elem);
		}
		else if(strcasecmp(elem->name,"ptime") == 0) {
			if(is_cdata_valid(elem) == TRUE) {
				rtp_settings->ptime = (apr_uint16_t)atol(cdata_text_get(elem));
			}
		}
		else if(strcasecmp(elem->name,"codecs") == 0) {
			const mpf_codec_manager_t *codec_manager = mrcp_client_codec_manager_get(loader->client);
			if(is_cdata_valid(elem) == TRUE && codec_manager) {
				mpf_codec_manager_codec_list_load(
					codec_manager,
					&rtp_settings->codec_list,
					cdata_text_get(elem),
					loader->pool);
			}
		}
		else if(strcasecmp(elem->name,"rtcp") == 0) {
			unimrcp_client_rtcp_settings_load(loader,rtp_settings,elem);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}    

	return mrcp_client_rtp_settings_register(loader->client,rtp_settings,id);
}

/** Load MRCPv2 profile */
static apt_bool_t unimrcp_client_mrcpv2_profile_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_profile_t *profile;
	mrcp_sig_agent_t *sip_agent = NULL;
	mrcp_connection_agent_t *mrcpv2_agent = NULL;
	mpf_engine_t *media_engine = NULL;
	mpf_termination_factory_t *rtp_factory = NULL;
	mpf_rtp_settings_t *rtp_settings = NULL;
	mrcp_sig_settings_t *sip_settings = NULL;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading MRCPv2 Profile <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);

		if(is_cdata_valid(elem) == FALSE) {
			continue;
		}

		if(strcasecmp(elem->name,"sip-uac") == 0) {
			sip_agent = mrcp_client_signaling_agent_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"mrcpv2-uac") == 0) {
			mrcpv2_agent = mrcp_client_connection_agent_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"media-engine") == 0) {
			media_engine = mrcp_client_media_engine_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"rtp-factory") == 0) {
			rtp_factory = mrcp_client_rtp_factory_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"sip-settings") == 0) {
			sip_settings = mrcp_client_signaling_settings_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"rtp-settings") == 0) {
			rtp_settings = mrcp_client_rtp_settings_get(loader->client,cdata_text_get(elem));
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	apt_log(APT_LOG_MARK,APT_PRIO_NOTICE,"Create MRCPv2 Profile [%s]",id);
	profile = mrcp_client_profile_create(
		NULL,sip_agent,mrcpv2_agent,
		media_engine,rtp_factory,
		rtp_settings,sip_settings,
		loader->pool);
	return mrcp_client_profile_register(loader->client,profile,id);
}

/** Load MRCPv1 profile */
static apt_bool_t unimrcp_client_mrcpv1_profile_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root, const char *id)
{
	const apr_xml_elem *elem;
	mrcp_profile_t *profile;
	mrcp_sig_agent_t *rtsp_agent = NULL;
	mpf_engine_t *media_engine = NULL;
	mpf_termination_factory_t *rtp_factory = NULL;
	mpf_rtp_settings_t *rtp_settings = NULL;
	mrcp_sig_settings_t *rtsp_settings = NULL;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading MRCPv1 Profile <%s>",id);
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);

		if(is_cdata_valid(elem) == FALSE) {
			continue;
		}

		if(strcasecmp(elem->name,"rtsp-uac") == 0) {
			rtsp_agent = mrcp_client_signaling_agent_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"media-engine") == 0) {
			media_engine = mrcp_client_media_engine_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"rtp-factory") == 0) {
			rtp_factory = mrcp_client_rtp_factory_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"rtsp-settings") == 0) {
			rtsp_settings = mrcp_client_signaling_settings_get(loader->client,cdata_text_get(elem));
		}
		else if(strcasecmp(elem->name,"rtp-settings") == 0) {
			rtp_settings = mrcp_client_rtp_settings_get(loader->client,cdata_text_get(elem));
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	apt_log(APT_LOG_MARK,APT_PRIO_NOTICE,"Create MRCPv1 Profile [%s]",id);
	profile = mrcp_client_profile_create(
		NULL,rtsp_agent,NULL,
		media_engine,rtp_factory,
		rtp_settings,rtsp_settings,
		loader->pool);
	return mrcp_client_profile_register(loader->client,profile,id);
}


/** Load properties */
static apt_bool_t unimrcp_client_properties_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Properties");
	for(elem = root->first_child; elem; elem = elem->next) {
		apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Element <%s>",elem->name);
		if(strcasecmp(elem->name,"ip") == 0) {
			loader->ip = unimrcp_client_ip_address_get(loader,elem,DEFAULT_IP_ADDRESS);
			apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Set Property ip:%s",loader->ip);
		}
		else if(strcasecmp(elem->name,"ext-ip") == 0) {
			loader->ext_ip = unimrcp_client_ip_address_get(loader,elem,DEFAULT_IP_ADDRESS);
			apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Set Property ext-ip:%s",loader->ext_ip);
		}
		else if(strcasecmp(elem->name,"server-ip") == 0) {
			loader->server_ip = unimrcp_client_ip_address_get(loader,elem,DEFAULT_IP_ADDRESS);
			apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Set Property server-ip:%s",loader->server_ip);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	if(!loader->ip) {
		loader->ip = DEFAULT_IP_ADDRESS;
	}
	if(!loader->server_ip) {
		loader->server_ip = loader->ip;
	}
	return TRUE;
}

/** Load components */
static apt_bool_t unimrcp_client_components_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;
	const apr_xml_attr *id_attr;
	const apr_xml_attr *enable_attr;
	const char *id;

	/* Create codec manager first (probably it should be loaded from config either) */
	mpf_codec_manager_t *codec_manager = mpf_engine_codec_manager_create(loader->pool);
	if(codec_manager) {
		mrcp_client_codec_manager_register(loader->client,codec_manager);
	}

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Components");
	for(elem = root->first_child; elem; elem = elem->next) {
		if(strcasecmp(elem->name,"resource-factory") == 0) {
			unimrcp_client_resource_factory_load(loader,elem);
			continue;
		}
		
		/* get common "id" and "enable" attributes */
		if(header_attribs_get(elem,&id_attr,&enable_attr) == FALSE) {
			/* invalid id */
			continue;
		}
		if(is_attr_enabled(enable_attr) == FALSE) {
			/* disabled element, just skip it */
			continue;
		}
		id = apr_pstrdup(loader->pool,id_attr->value);

		if(strcasecmp(elem->name,"sip-uac") == 0) {
			unimrcp_client_sip_uac_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"rtsp-uac") == 0) {
			unimrcp_client_rtsp_uac_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"mrcpv2-uac") == 0) {
			unimrcp_client_mrcpv2_uac_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"media-engine") == 0) {
			unimrcp_client_media_engine_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"rtp-factory") == 0) {
			unimrcp_client_rtp_factory_load(loader,elem,id);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}    
	return TRUE;
}

/** Load settings */
static apt_bool_t unimrcp_client_settings_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;
	const apr_xml_attr *id_attr;
	const apr_xml_attr *enable_attr;
	const char *id;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Settings");
	for(elem = root->first_child; elem; elem = elem->next) {
		/* get common "id" and "enable" attributes */
		if(header_attribs_get(elem,&id_attr,&enable_attr) == FALSE) {
			/* invalid id */
			continue;
		}
		if(is_attr_enabled(enable_attr) == FALSE) {
			/* disabled element, just skip it */
			continue;
		}
		id = apr_pstrdup(loader->pool,id_attr->value);

		if(strcasecmp(elem->name,"sip-settings") == 0) {
			unimrcp_client_sip_settings_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"rtsp-settings") == 0) {
			unimrcp_client_rtsp_settings_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"rtp-settings") == 0) {
			unimrcp_client_rtp_settings_load(loader,elem,id);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}    
	return TRUE;
}

/** Load profiles */
static apt_bool_t unimrcp_client_profiles_load(unimrcp_client_loader_t *loader, const apr_xml_elem *root)
{
	const apr_xml_elem *elem;
	const apr_xml_attr *id_attr;
	const apr_xml_attr *enable_attr;
	const char *id;

	apt_log(APT_LOG_MARK,APT_PRIO_DEBUG,"Loading Profiles");
	for(elem = root->first_child; elem; elem = elem->next) {
		/* get common "id" and "enable" attributes */
		if(header_attribs_get(elem,&id_attr,&enable_attr) == FALSE) {
			/* invalid id */
			continue;
		}
		if(is_attr_enabled(enable_attr) == FALSE) {
			/* disabled element, just skip it */
			continue;
		}
		id = apr_pstrdup(loader->pool,id_attr->value);

		if(strcasecmp(elem->name,"mrcpv2-profile") == 0) {
			unimrcp_client_mrcpv2_profile_load(loader,elem,id);
		}
		else if(strcasecmp(elem->name,"mrcpv1-profile") == 0) {
			unimrcp_client_mrcpv1_profile_load(loader,elem,id);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}    
	return TRUE;
}

/** Parse XML document */
static apr_xml_doc* unimrcp_client_doc_parse(const char *file_path, apr_pool_t *pool)
{
	apr_xml_parser *parser = NULL;
	apr_xml_doc *xml_doc = NULL;
	apr_file_t *fd = NULL;
	apr_status_t rv;

	apt_log(APT_LOG_MARK,APT_PRIO_NOTICE,"Open Config File [%s]",file_path);
	rv = apr_file_open(&fd,file_path,APR_READ|APR_BINARY,0,pool);
	if(rv != APR_SUCCESS) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Failed to Open Config File [%s]",file_path);
		return NULL;
	}

	rv = apr_xml_parse_file(pool,&parser,&xml_doc,fd,XML_FILE_BUFFER_LENGTH);
	if(rv != APR_SUCCESS) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Failed to Parse Config File [%s]",file_path);
		xml_doc = NULL;
	}
	
	apr_file_close(fd);
	return xml_doc;
}

/** Process parsed XML document */
static apt_bool_t unimrcp_client_doc_process(unimrcp_client_loader_t *loader, const char *dir_path, apr_xml_doc *doc, apr_pool_t *pool)
{
	const apr_xml_elem *elem;
	const apr_xml_elem *root;
	const apr_xml_attr *attr;
	const char *version = NULL;
	const char *subfolder = NULL;

	if(!doc) {
		return FALSE;
	}

	root = doc->root;

	/* Match document name */
	if(!root || strcasecmp(root->name,"unimrcpclient") != 0) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Document <%s>",root->name);
		return FALSE;
	}

	/* Read attributes */
	for(attr = root->attr; attr; attr = attr->next) {
		if(strcasecmp(attr->name,"version") == 0) {
			version = attr->value;
		}
		else if(strcasecmp(attr->name,"subfolder") == 0) {
			subfolder = attr->value;
		}
	}

	/* Check version number first */
	if(!version) {
		apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Version");
		return FALSE;
	}

	loader->doc = doc;

	/* Navigate through document */
	for(elem = root->first_child; elem; elem = elem->next) {
		if(strcasecmp(elem->name,"properties") == 0) {
			unimrcp_client_properties_load(loader,elem);
		}
		else if(strcasecmp(elem->name,"components") == 0) {
			unimrcp_client_components_load(loader,elem);
		}
		else if(strcasecmp(elem->name,"settings") == 0) {
			unimrcp_client_settings_load(loader,elem);
		}
		else if(strcasecmp(elem->name,"profiles") == 0) {
			unimrcp_client_profiles_load(loader,elem);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Unknown Element <%s>",elem->name);
		}
	}

	if(subfolder && subfolder != '\0') {
		apr_dir_t *dir;
		apr_finfo_t finfo;
		apr_status_t rv;

		if (!dir_path) {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"Attempt to Process Subdirectory when "
				"Creating from Config String");
			return TRUE;
		}

		dir_path = apr_psprintf(pool,"%s/%s",dir_path,subfolder);
		apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Enter Directory [%s]",dir_path);
		rv = apr_dir_open(&dir,dir_path,pool);
		if(rv == APR_SUCCESS) {
			while(apr_dir_read(&finfo, APR_FINFO_NAME, dir) == APR_SUCCESS) {
				if(apr_fnmatch("*.xml", finfo.name, 0) == APR_SUCCESS) {
					unimrcp_client_load(loader,dir_path,finfo.name);
				}
			}
			apr_dir_close(dir);
			apt_log(APT_LOG_MARK,APT_PRIO_INFO,"Leave Directory [%s]",dir_path);
		}
		else {
			apt_log(APT_LOG_MARK,APT_PRIO_WARNING,"No Such Directory %s",dir_path);
		}
	}
	return TRUE;
}

/** Load UniMRCP client from file */
static apt_bool_t unimrcp_client_load(unimrcp_client_loader_t *loader, const char *dir_path, const char *file_name)
{
	apr_pool_t *pool = loader->pool;
	apr_xml_doc *doc;
	const char *file_path;

	if(!dir_path || !file_name) {
		return FALSE;
	}

	if(*dir_path == '\0') {
		file_path = file_name;
	}
	else {
		file_path = apr_psprintf(pool,"%s/%s",dir_path,file_name);
	}

	/* Parse XML document */
	doc = unimrcp_client_doc_parse(file_path,pool);
	return unimrcp_client_doc_process(loader, dir_path, doc, pool);
}

/** Read configuration from string */
static apt_bool_t unimrcp_client_load2(unimrcp_client_loader_t *loader, const char *xmlconfig)
{
	apr_pool_t *pool = loader->pool;
	apr_xml_parser *parser;
	apr_xml_doc *xml_doc;
	char errbuf[4096];
	apr_status_t rv;

	parser = apr_xml_parser_create(pool);
	if (!parser) return FALSE;
	rv = apr_xml_parser_feed(parser, xmlconfig, strlen(xmlconfig));
	if (rv != APR_SUCCESS) {
		apr_xml_parser_geterror(parser, errbuf, sizeof(errbuf));
		apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error parsing XML configuration: %d %pm: %s",
			rv, &rv, errbuf);
		return FALSE;
	}
	rv = apr_xml_parser_done(parser, &xml_doc);
	if (rv != APR_SUCCESS) {
		apr_xml_parser_geterror(parser, errbuf, sizeof(errbuf));
		apt_log(APT_LOG_MARK, APT_PRIO_ERROR, "Error parsing XML configuration: %d %pm: %s",
			rv, &rv, errbuf);
		return FALSE;
	}
	return unimrcp_client_doc_process(loader, NULL, xml_doc, pool);
}
