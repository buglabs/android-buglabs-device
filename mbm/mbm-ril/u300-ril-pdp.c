/* ST-Ericsson U300 RIL
**
** Copyright (C) ST-Ericsson AB 2008-2009
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
**
** Based on reference-ril by The Android Open Source Project.
**
** Heavily modified for ST-Ericsson U300 modems.
** Author: Christian Bejram <christian.bejram@stericsson.com>
**
*/

#include <stdio.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include <telephony/ril.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/route.h>
#include <cutils/properties.h>
#include "u300-ril-error.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#include "u300-ril.h"
#include "net-utils.h"

/* Allocate and create an UCS-2 format string */
static char *ucs2StringCreate(const char *String);

/* Last pdp fail cause */
static int s_lastPdpFailCause = PDP_FAIL_ERROR_UNSPECIFIED;

#define MBM_ENAP_WAIT_TIME 17*5	/* loops to wait CONNECTION aprox 17s */

void requestOrSendPDPContextList(RIL_Token *token)
{
    ATResponse *atresponse = NULL;
    RIL_Data_Call_Response response;
    int e2napState = getE2napState();
    int err;
    int cid;
    char *line, *apn, *type, *address;

    memset(&response, 0, sizeof(response));

    err = at_send_command_multiline("AT+CGDCONT?", "+CGDCONT:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &cid);
    if (err < 0)
        goto error;

    response.cid = cid;

    if (e2napState == E2NAP_ST_CONNECTED)
        response.active = 1;

    err = at_tok_nextstr(&line, &type);
    if (err < 0)
        goto error;

    response.type = alloca(strlen(type) + 1);
    strcpy(response.type, type);

    err = at_tok_nextstr(&line, &apn);
    if (err < 0)
        goto error;

    response.apn = alloca(strlen(apn) + 1);
    strcpy(response.apn, apn);

    err = at_tok_nextstr(&line, &address);
    if (err < 0)
        goto error;

    response.address = alloca(strlen(address) + 1);
    strcpy(response.address, address);

    if (token != NULL)
        RIL_onRequestComplete(*token, RIL_E_SUCCESS, &response,
                              sizeof(RIL_Data_Call_Response));
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, &response,
                                  sizeof(RIL_Data_Call_Response));

    at_response_free(atresponse);
    return;

error:
    if (token != NULL)
        RIL_onRequestComplete(*token, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onUnsolicitedResponse(RIL_UNSOL_DATA_CALL_LIST_CHANGED, NULL, 0);

    at_response_free(atresponse);
}

/**
 * RIL_UNSOL_PDP_CONTEXT_LIST_CHANGED
 *
 * Indicate a PDP context state has changed, or a new context
 * has been activated or deactivated.
*
 * See also: RIL_REQUEST_PDP_CONTEXT_LIST
 */
void onPDPContextListChanged(void *param)
{
    (void) param;
    requestOrSendPDPContextList(NULL);
}

/**
 * RIL_REQUEST_PDP_CONTEXT_LIST
 *
 * Queries the status of PDP contexts, returning for each
 * its CID, whether or not it is active, and its PDP type,
 * APN, and PDP adddress.
*/
void requestPDPContextList(void *data, size_t datalen, RIL_Token t)
{
    (void) data;
    (void) datalen;
    requestOrSendPDPContextList(&t);
}

void mbm_check_error_cause()
{
    int e2napCause = getE2napCause();
    int e2napState = getE2napState();

    if ((e2napCause < E2NAP_C_SUCCESS) ||
	(e2napState == E2NAP_ST_CONNECTED)) {
	s_lastPdpFailCause = PDP_FAIL_ERROR_UNSPECIFIED;
	return;
    }

    /* Protocol errors from 95 - 111
     * Standard defines only 95 - 101 and 111
     * Those 102-110 are missing
     */
    if (e2napCause >= GRPS_SEM_INCORRECT_MSG &&
	e2napCause <= GPRS_MSG_NOT_COMP_PROTO_STATE) {
	s_lastPdpFailCause = PDP_FAIL_PROTOCOL_ERRORS;
	LOGD("Connection error: %s cause %s",
	     e2napStateToString(e2napState),
	     errorCauseToString(e2napCause));
	return;
    }

    if (e2napCause == GPRS_PROTO_ERROR_UNSPECIFIED) {
	s_lastPdpFailCause = PDP_FAIL_PROTOCOL_ERRORS;
	LOGD("Connection error: %s cause %s",
	     e2napStateToString(e2napState),
	     errorCauseToString(e2napCause));
	return;
    }
}

void requestSetupDefaultPDP(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    in_addr_t addr = 0;
    in_addr_t gateway = 0;
    in_addr_t dns1 = 0;
    in_addr_t dns2 = 0;

    const char *apn, *user, *pass, *auth;
    char *cmd = NULL;
    char *atAuth = NULL;
    char *atUser = NULL, *atPass = NULL, *origChSet = NULL, *chSet = NULL;
    char *line = NULL;
    char *ipAddrStr = NULL;
    char *p = NULL;

    int err = -1;
    int cme_err, i;
    int n = 0;
    int dnscnt = 0;
    char *response[3] = { "1", "usb0", "0.0.0.0" };
    int e2napState = setE2napState(-1);
    int e2napCause = setE2napCause(-1);

    (void) data;
    (void) datalen;

    apn = ((const char **) data)[2];
    user = ((const char **) data)[3];
    pass = ((const char **) data)[4];
    auth = ((const char **) data)[5];

    s_lastPdpFailCause = PDP_FAIL_ERROR_UNSPECIFIED;

    LOGD("requestSetupDefaultPDP: requesting data connection to APN '%s'",
	 apn);

    if (ifc_init()) {
	LOGE("requestSetupDefaultPDP: FAILED to set up ifc!");
	goto error;
    }

    if (ifc_down(ril_iface)) {
	LOGE("requestSetupDefaultPDP: Failed to bring down %s!",
	     ril_iface);
	goto error;
    }

    asprintf(&cmd, "AT+CGDCONT=1,\"IP\",\"%s\"", apn);
    err = at_send_command(cmd, &p_response);
    if (err < 0 || (p_response == NULL) || p_response->success == 0) {
	cme_err = at_get_cme_error(p_response);
	LOGE("requestSetupDefaultPDP: CGDCONT failed: %d, cme: %d", err,
	     cme_err);
	goto error;
    }
    at_response_free(p_response);
    p_response = NULL;
    free(cmd);
    cmd = NULL;

    /* Set authentication protocol */
    if (0 == strcmp("0", auth))
	/* PAP never performed; CHAP never performed */
	asprintf(&atAuth, "00001");
    else if (0 == strcmp("1", auth))
	/* PAP may be performed; CHAP never performed */
	asprintf(&atAuth, "00011");
    else if (0 == strcmp("2", auth))
	/* PAP never performed; CHAP may be performed */
	asprintf(&atAuth, "00101");
    else if (0 == strcmp("3", auth))
	/* PAP may be performed; CHAP may be performed. */
	asprintf(&atAuth, "00111");
    else {
	LOGE("requestSetupDefaultPDP: Unrecognized authentication type %s. Using default value (CHAP, PAP and None).", auth);
	asprintf(&atAuth, "00111");
    }

    /* Because of module FW issues, some characters need UCS-2 format to be supported
     * in the user and pass strings. Read current setting, change to UCS-2 format,
     * send *EIAAUW command, and finally change back to previous character set.
     */
    err = at_send_command_singleline("AT+CSCS?", "+CSCS:", &p_response);
    if (err < 0 || (p_response == NULL) || (p_response->success == 0)) {
        LOGE("requestSetupDefaultPDP: Failed to read AT+CSCS?");
        goto error;
    }

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &chSet);
    if (err < 0)
        goto error;

    /* If not any of the listed below, assume already UCS-2 */
    if ((0 == strcmp(chSet, "GSM")) || (0 == strcmp(chSet, "IRA")) ||
        (0 == strncmp(chSet, "8859",4)) || (0 == strcmp(chSet, "UTF-8"))) {
        asprintf(&origChSet, "%s", chSet);

        /* Set UCS-2 character set */
        err = at_send_command("AT+CSCS=\"UCS2\"", NULL);
        if (err < 0) {
            LOGE("requestSetupDefaultPDP: Failed to set AT+CSCS=UCS2");
            free(origChSet);
            goto error;
        }
    }
    else
        asprintf(&origChSet, "UCS2");

    at_response_free(p_response);
    p_response = NULL;
    line = NULL;
    chSet = NULL;

    atUser = ucs2StringCreate(user);
    atPass = ucs2StringCreate(pass);
    asprintf(&cmd, "AT*EIAAUW=1,1,\"%s\",\"%s\",%s", atUser, atPass, atAuth);
    free(atUser);
    free(atPass);
    free(atAuth);
    atUser = NULL;
    atPass = NULL;
    atAuth = NULL;
    err = at_send_command(cmd, NULL);
    if (err < 0)
	goto error;
    free(cmd);
    cmd = NULL;

    /* Set back to the original character set */
    chSet = ucs2StringCreate(origChSet);
    asprintf(&cmd, "AT+CSCS=\"%s\"", chSet);
    free(chSet);
    chSet = NULL;
    err = at_send_command(cmd, NULL);
    if (err < 0) {
        LOGE("requestSetupDefaultPDP: Failed to set back character set to %s", origChSet);
        free(origChSet);
        origChSet = NULL;
        goto error;
    }
    free(origChSet);
    origChSet = NULL;
    free(cmd);
    cmd = NULL;

    /* Start data on PDP context 1 */
    err = at_send_command("AT*ENAP=1,1", &p_response);
    if (err < 0 || (p_response == NULL) || p_response->success == 0) {
	cme_err = at_get_cme_error(p_response);
	LOGE("requestSetupDefaultPDP: ENAP failed: %d  cme: %d", err,
	     cme_err);
	goto error;
    }
    at_response_free(p_response);
    p_response = NULL;

    for (i = 0; i < MBM_ENAP_WAIT_TIME; i++) {
	e2napState = getE2napState();

	if (e2napState == E2NAP_ST_CONNECTED ||
	    e2napState == E2NAP_ST_DISCONNECTED) {
	    LOGD("requestSetupDefaultPDP: %s",
		 e2napStateToString(e2napState));
	    break;
	}
	usleep(200 * 1000);
    }

    e2napState = getE2napState();
    e2napCause = getE2napCause();

    if (e2napState == E2NAP_ST_DISCONNECTED)
	goto error;

    /* *E2IPCFG:
     *  (1,"10.155.68.129")(2,"10.155.68.131")(3,"80.251.192.244")(3,"80.251.192.245")
     */
    err = at_send_command_singleline("AT*E2IPCFG?", "*E2IPCFG:",
				     &p_response);
    if (err < 0 || p_response->success == 0
	|| p_response->p_intermediates == NULL)
	goto error;

    p = p_response->p_intermediates->line;
    while (*p != '\0') {
	if (*p == '(')
	    n++;
	p++;
    }

    p = p_response->p_intermediates->line;

    /* Loop and collect information */
    for (i = 0; i < n; i++) {
	int stat = 0;
	char *line = NULL;
	char *address = NULL;
	char *remaining = NULL;

	line = getFirstElementValue(p, "(", ")", &remaining);
	p = remaining;

	if (line == NULL) {
	    LOGD("requestSetupDefaultPDP: No more connection info.");
	    break;
	}
	/* <stat> */
	err = at_tok_nextint(&line, &stat);
	if (err < 0)
	    goto error;

	/* <address> */
	err = at_tok_nextstr(&line, &address);
	if (err < 0)
	    goto error;

	if (stat == 1) {
	    ipAddrStr = address;
	    LOGD("requestSetupDefaultPDP: IP Address: %s\n", address);
	    if (inet_pton(AF_INET, address, &addr) <= 0) {
		LOGE("requestSetupDefaultPDP: inet_pton() failed for %s!",
		     address);
		goto error;
	    }
	}

	if (stat == 2) {
	    LOGD("requestSetupDefaultPDP: GW: %s\n", address);
	    if (inet_pton(AF_INET, address, &gateway) <= 0) {
		LOGE("requestSetupDefaultPDP: Failed inet_pton for gw %s!",
		     address);
		goto error;
	    }

	}

	if (stat == 3) {
	    dnscnt++;
	    if (dnscnt == 1) {
		if (inet_pton(AF_INET, address, &dns1) <= 0) {
		    LOGE("requestSetupDefaultPDP: Failed inet_pton for gw %s!", address);
		    goto error;
		}
	    } else if (dnscnt == 2) {
		if (inet_pton(AF_INET, address, &dns2) <= 0) {
		    LOGE("requestSetupDefaultPDP: Failed inet_pton for gw %s!", address);
		    goto error;
		}

	    }

	}
    }

    LOGI("requestSetupDefaultPDP: Setting up interface.");
    e2napState = getE2napState();

    if (e2napState == E2NAP_ST_DISCONNECTED)
	goto error;		/* we got disconnected */

    /* Don't use android netutils. We use our own and get the routing correct.
       Carl Nordbeck */
    if (ifc_configure(ril_iface, addr, gateway, dns1, dns1)) {
	LOGE("requestSetupDefaultPDP: Failed to configure the interface %s", ril_iface);
    }

    response[1] = ril_iface;
    response[2] = ipAddrStr;

    e2napState = getE2napState();
    LOGD("requestSetupDefaultPDP: IP Address %s, %s", ipAddrStr,
	 e2napStateToString(e2napState));

    if (e2napState == E2NAP_ST_DISCONNECTED)
	goto error;		/* we got disconnected */

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
			      NULL, 0);
    at_response_free(p_response);
    free(cmd);

    return;

  error:

    mbm_check_error_cause();

    at_response_free(p_response);
    /* try to restore enap state */
    err = at_send_command("AT*ENAP=0", &p_response);

    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
    free(cmd);
}

/* CHECK There are several error cases if PDP deactivation fails
 * 24.008: 8, 25, 36, 38, 39, 112
 */
void requestDeactivateDefaultPDP(void *data, size_t datalen, RIL_Token t)
{
    ATResponse *p_response = NULL;
    int enap = 0;
    int err, i;
    char *line;
    (void) data;
    (void) datalen;

    err = at_send_command_singleline("AT*ENAP?", "*ENAP:", &p_response);
    if (err < 0 || p_response->success == 0)
	goto error;

    line = p_response->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
	goto error;

    err = at_tok_nextint(&line, &enap);
    if (err < 0)
	goto error;

    if (enap == ENAP_T_CONN_IN_PROG)
	LOGE("requestDeactivateDefaultPDP: When deactivating PDP, enap is IN_PROGRESS");

    if (enap == ENAP_T_CONNECTED) {
	err = at_send_command("AT*ENAP=0", NULL);	/* TODO: can return CME error */

	if (err < 0)
	    goto error;
	for (i = 0; i < MBM_ENAP_WAIT_TIME; i++) {
	    err =
		at_send_command_singleline("AT*ENAP?", "*ENAP:",
					   &p_response);

	    if (err < 0 || p_response->success == 0)
		goto error;

	    line = p_response->p_intermediates->line;

	    err = at_tok_start(&line);
	    if (err < 0)
		goto error;

	    err = at_tok_nextint(&line, &enap);
	    if (err < 0)
		goto error;

	    if (enap == 0)
		break;

	    sleep(1);
	}

	if (enap != ENAP_T_NOT_CONNECTED)
	    goto error;

	/* Bring down the interface as well. */
	if (ifc_init())
	    goto error;

	if (ifc_down(ril_iface))
	    goto error;

	ifc_close();
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    at_response_free(p_response);
    return;

  error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(p_response);
}

/**
 * RIL_REQUEST_LAST_PDP_FAIL_CAUSE
 *
 * Requests the failure cause code for the most recently failed PDP
 * context activate.
 *
 * See also: RIL_REQUEST_LAST_CALL_FAIL_CAUSE.
 *
 */
void requestLastPDPFailCause(void *data, size_t datalen, RIL_Token t)
{
    (void) data;
    (void) datalen;
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &s_lastPdpFailCause,
			  sizeof(int));
}

/**
 * Returns a pointer to allocated memory filled with AT command
 * UCS-2 formatted string corresponding to the input string.
 * Note: Caller need to take care of freeing the
 *  allocated memory by calling free( ) when the
 *  created string is no longer used.
 */
static char *ucs2StringCreate(const char *iString)
{
    int slen = 0;
    int idx = 0;
    char *ucs2String = NULL;

    /* In case of NULL input, create an empty string as output */
    if (NULL == iString)
        slen = 0;
    else
        slen = strlen(iString);

    ucs2String = (char *)malloc(sizeof(char)*(slen*4+1));
    for (idx = 0; idx < slen; idx++)
        sprintf(&ucs2String[idx*4], "%04x", iString[idx]);
    ucs2String[idx*4] = '\0';
    return ucs2String;
}
