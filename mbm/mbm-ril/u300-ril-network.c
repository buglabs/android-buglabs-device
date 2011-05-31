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
*/

#include <stdio.h>
#include <telephony/ril.h>
#include <assert.h>
#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"
#include "u300-ril.h"
#include "u300-ril-error.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define REPOLL_OPERATOR_SELECTED 30     /* 30 * 2 = 1M = ok? */
static const struct timeval TIMEVAL_OPERATOR_SELECT_POLL = { 2, 0 };

static void pollOperatorSelected(void *params);

/*
 * s_registrationDeniedReason is used to keep track of registration deny
 * reason for which is called by pollOperatorSelected from
 * RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC, so that in case
 * of invalid SIM/ME, Android will not continuously poll for operator.
 *
 * s_registrationDeniedReason is set when receives the registration deny
 * and detail reason from "AT*E2REG?" command, and is reset to
 * DEFAULT_VALUE otherwise.
 */
static Reg_Deny_DetailReason s_registrationDeniedReason = DEFAULT_VALUE;


struct operatorPollParams {
    RIL_Token t;
    int loopcount;
};

/* +CGREG AcT values */
enum CREG_AcT {
    CGREG_ACT_GSM               = 0,
    CGREG_ACT_GSM_COMPACT       = 1, /* Not Supported */
    CGREG_ACT_UTRAN             = 2,
    CGREG_ACT_GSM_EGPRS         = 3,
    CGREG_ACT_UTRAN_HSDPA       = 4,
    CGREG_ACT_UTRAN_HSUPA       = 5,
    CGREG_ACT_UTRAN_HSUPA_HSDPA = 6
};

/* +CGREG stat values */
enum CREG_stat {
    CGREG_STAT_NOT_REG            = 0,
    CGREG_STAT_REG_HOME_NET       = 1,
    CGREG_STAT_NOT_REG_SEARCHING  = 2,
    CGREG_STAT_REG_DENIED         = 3,
    CGREG_STAT_UKNOWN             = 4,
    CGREG_STAT_ROAMING            = 5
};

/* *ERINFO umts_info values */
enum ERINFO_umts {
    ERINFO_UMTS_NO_UMTS_HSDPA     = 0,
    ERINFO_UMTS_UMTS              = 1,
    ERINFO_UMTS_HSDPA             = 2,
    ERINFO_UMTS_HSPA_EVOL         = 3
};


/**
 * Poll +COPS? and return a success, or if the loop counter reaches
 * REPOLL_OPERATOR_SELECTED, return generic failure.
 */
static void pollOperatorSelected(void *params)
{
    int err = 0;
    int response = 0;
    char *line = NULL;
    ATResponse *atresponse = NULL;
    struct operatorPollParams *poll_params;
    RIL_Token t;

    assert(params != NULL);

    poll_params = (struct operatorPollParams *) params;
    t = poll_params->t;

    if (poll_params->loopcount >= REPOLL_OPERATOR_SELECTED) {
        goto error;
    }

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);
    if (err < 0)
        goto error;

    /* If we don't get more than the COPS: {0-4} we are not registered.
       Loop and try again. */
    if (!at_tok_hasmore(&line)) {
        switch (s_registrationDeniedReason) {
        case IMSI_UNKNOWN_IN_HLR: /* fall through */
        case ILLEGAL_ME:
            RIL_onRequestComplete(t, RIL_E_ILLEGAL_SIM_OR_ME, NULL, 0);
            free(poll_params);
            break;
        default:
            poll_params->loopcount++;
            enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollOperatorSelected,
                            poll_params, &TIMEVAL_OPERATOR_SELECT_POLL);
        }
    } else {
        /* We got operator, throw a success! */
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
        free(poll_params);
    }

    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    free(poll_params);
    at_response_free(atresponse);
    return;
}

/**
 * RIL_UNSOL_NITZ_TIME_RECEIVED
 *
 * Called when radio has received a NITZ time message.
*/
void onNetworkTimeReceived(const char *s)
{
    /* TODO: We don't get any DST data here, don't know how we get it
             either. Needs some investigation. */
    char *line, *tok, *response, *tz, *nitz;

    tok = line = strdup(s);
    at_tok_start(&tok);

    if (at_tok_nextstr(&tok, &tz) != 0) {
        LOGE("Failed to parse NITZ line %s\n", s);
    } else if (at_tok_nextstr(&tok, &nitz) != 0) {
        LOGE("Failed to parse NITZ line %s\n", s);
    } else {
        asprintf(&response, "%s%s", nitz + 2, tz);

        RIL_onUnsolicitedResponse(RIL_UNSOL_NITZ_TIME_RECEIVED,
                                  response, sizeof(char *));

        free(response);
    }

    free(line);
}

/**
 * RIL_UNSOL_SIGNAL_STRENGTH
 *
 * Radio may report signal strength rather han have it polled.
 *
 * "data" is a const RIL_SignalStrength *
 */
void pollSignalStrength(void *bar)
{
    ATResponse *atresponse = NULL;
    RIL_SignalStrength signalStrength;
    char *line = NULL;
    int err;
    int rssi, ber;

    memset(&signalStrength, 0, sizeof(RIL_SignalStrength));

    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line,&rssi);
    if (err < 0)
        goto error;
    signalStrength.GW_SignalStrength.signalStrength = rssi;

    err = at_tok_nextint(&line, &ber);
    if (err < 0)
        goto error;
    signalStrength.GW_SignalStrength.bitErrorRate = ber;

    at_response_free(atresponse);
    atresponse = NULL;

    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
                              &signalStrength, sizeof(RIL_SignalStrength));
    at_response_free(atresponse);
    return;

error:
    signalStrength.GW_SignalStrength.signalStrength = (int)bar;
    signalStrength.GW_SignalStrength.bitErrorRate = 99;
  
    RIL_onUnsolicitedResponse(RIL_UNSOL_SIGNAL_STRENGTH,
                              &signalStrength, sizeof(RIL_SignalStrength));
    
    at_response_free(atresponse);
}
void onSignalStrengthChanged(const char *s)
{
    int err;
    int skip;
    int bars = 0;
    char *line = NULL;

    line = strdup(s);
    if (line == NULL)
        goto error;

    at_tok_start(&line);

    err = at_tok_nextint(&line, &skip);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &bars);
    if (err < 0)
        goto error;

    /* This is the faked value if we can't get the correct one */
    if (bars > 0) {
        bars *= 4;
        bars--;
    }

error:
    free(line);
    enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollSignalStrength, (void *)bars, NULL);
}

/**
 * RIL_REQUEST_SET_BAND_MODE
 *
 * Assign a specified band for RF configuration.
*/
void requestSetBandMode(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int bandMode = ((int *)data)[0];

    /* Currently only allow automatic. */
    if (bandMode == 0)
        RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE
 *
 * Query the list of band mode supported by RF.
 *
 * See also: RIL_REQUEST_SET_BAND_MODE
 */
void requestQueryAvailableBandMode(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    int response[2];

    response[0] = 2;
    response[1] = 0;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));
}

/**
 * RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC
 *
 * Specify that the network should be selected automatically.
*/
void requestSetNetworkSelectionAutomatic(void *data, size_t datalen,
                                         RIL_Token t)
{
    (void) data; (void) datalen;
    int err = 0;
    struct operatorPollParams *poll_params = NULL;

    err = at_send_command("AT+COPS=0", NULL);
    if (err < 0)
        goto error;

    poll_params = malloc(sizeof(struct operatorPollParams));

    poll_params->loopcount = 0;
    poll_params->t = t;

    enqueueRILEvent(RIL_EVENT_QUEUE_NORMAL, pollOperatorSelected,
                    poll_params, &TIMEVAL_OPERATOR_SELECT_POLL);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;
}

/**
 * RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL
 *
 * Manually select a specified network.
 *
 * The radio baseband/RIL implementation is expected to fall back to 
 * automatic selection mode if the manually selected network should go
 * out of range in the future.
 */
void requestSetNetworkSelectionManual(void *data, size_t datalen,
                                      RIL_Token t)
{
    /* 
     * AT+COPS=[<mode>[,<format>[,<oper>[,<AcT>]]]]
     *    <mode>   = 4 = Manual (<oper> field shall be present and AcT optionally) with fallback to automatic if manual fails.
     *    <format> = 2 = Numeric <oper>, the number has structure:
     *                   (country code digit 3)(country code digit 2)(country code digit 1)
     *                   (network code digit 2)(network code digit 1) 
     */

    (void) datalen;
    int err = 0;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    const char *mccMnc = (const char *) data;

    /* Check inparameter. */
    if (mccMnc == NULL) {
        goto error;
    }
    /* Build and send command. */
    asprintf(&cmd, "AT+COPS=1,2,\"%s\"", mccMnc);
    err = at_send_command(cmd, &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);
finally:

    at_response_free(atresponse);

    if (cmd != NULL)
        free(cmd);

    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_AVAILABLE_NETWORKS
 *
 * Scans for available networks.
*/
void requestQueryAvailableNetworks(void *data, size_t datalen, RIL_Token t)
{
    /* 
     * AT+COPS=?
     *   +COPS: [list of supported (<stat>,long alphanumeric <oper>
     *           ,short alphanumeric <oper>,numeric <oper>[,<AcT>])s]
     *          [,,(list of supported <mode>s),(list of supported <format>s)]
     *
     *   <stat>
     *     0 = unknown
     *     1 = available
     *     2 = current
     *     3 = forbidden 
     */
    (void) data; (void) datalen;
    int err = 0;
    ATResponse *atresponse = NULL;
    const char *statusTable[] =
        { "unknown", "available", "current", "forbidden" };
    char **responseArray = NULL;
    char *p;
    int n = 0;
    int i = 0;

    err = at_send_command_multiline("AT+COPS=?", "+COPS:", &atresponse);
    if (err < 0 || 
        atresponse->success == 0 || 
        atresponse->p_intermediates == NULL)
        goto error;

    p = atresponse->p_intermediates->line;
    while (*p != '\0') {
        if (*p == '(')
            n++;
        p++;
    }

    /* Allocate array of strings, blocks of 4 strings. */
    responseArray = alloca(n * 4 * sizeof(char *));

    p = atresponse->p_intermediates->line;

    /* Loop and collect response information into the response array. */
    for (i = 0; i < n; i++) {
        int status = 0;
        char *line = NULL;
        char *s = NULL;
        char *longAlphaNumeric = NULL;
        char *shortAlphaNumeric = NULL;
        char *numeric = NULL;
        char *remaining = NULL;

        s = line = getFirstElementValue(p, "(", ")", &remaining);
        p = remaining;

        if (line == NULL) {
            LOGE("Null pointer while parsing COPS response. This should not happen.");
            break;
        }
        /* <stat> */
        err = at_tok_nextint(&line, &status);
        if (err < 0)
            goto error;

        /* long alphanumeric <oper> */
        err = at_tok_nextstr(&line, &longAlphaNumeric);
        if (err < 0)
            goto error;

        /* short alphanumeric <oper> */            
        err = at_tok_nextstr(&line, &shortAlphaNumeric);
        if (err < 0)
            goto error;

        /* numeric <oper> */
        err = at_tok_nextstr(&line, &numeric);
        if (err < 0)
            goto error;

        responseArray[i * 4 + 0] = alloca(strlen(longAlphaNumeric) + 1);
        strcpy(responseArray[i * 4 + 0], longAlphaNumeric);

        responseArray[i * 4 + 1] = alloca(strlen(shortAlphaNumeric) + 1);
        strcpy(responseArray[i * 4 + 1], shortAlphaNumeric);

        responseArray[i * 4 + 2] = alloca(strlen(numeric) + 1);
        strcpy(responseArray[i * 4 + 2], numeric);

        free(s);

        /* 
         * Check if modem returned an empty string, and fill it with MNC/MMC 
         * if that's the case.
         */
        if (responseArray[i * 4 + 0] && strlen(responseArray[i * 4 + 0]) == 0) {
            responseArray[i * 4 + 0] = alloca(strlen(responseArray[i * 4 + 2])
                                              + 1);
            strcpy(responseArray[i * 4 + 0], responseArray[i * 4 + 2]);
        }

        if (responseArray[i * 4 + 1] && strlen(responseArray[i * 4 + 1]) == 0) {
            responseArray[i * 4 + 1] = alloca(strlen(responseArray[i * 4 + 2])
                                              + 1);
            strcpy(responseArray[i * 4 + 1], responseArray[i * 4 + 2]);
        }

        responseArray[i * 4 + 3] = alloca(strlen(statusTable[status]) + 1);
        sprintf(responseArray[i * 4 + 3], "%s", statusTable[status]);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseArray,
                          i * 4 * sizeof(char *));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE
 *
 * Requests to set the preferred network type for searching and registering
 * (CS/PS domain, RAT, and operation mode).
 */
void requestSetPreferredNetworkType(void *data, size_t datalen,
                                    RIL_Token t)
{
    (void) datalen;
    ATResponse *atresponse = NULL;
    int err = 0;
    int rat;
    int arg;
    char *cmd = NULL;
    RIL_Errno errno = RIL_E_GENERIC_FAILURE;

    rat = ((int *) data)[0];

    switch (rat) {
    case 0:
        arg = 1;
        break;
    case 1:
        arg = 5;
        break;
    case 2:
        arg = 6;
        break;
    default:
        errno = RIL_E_MODE_NOT_SUPPORTED;
        goto error;
    }

    asprintf(&cmd, "AT+CFUN=%d", arg);

    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, errno, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE
 *
 * Query the preferred network type (CS/PS domain, RAT, and operation mode)
 * for searching and registering.
 */
void requestGetPreferredNetworkType(void *data, size_t datalen,
                                    RIL_Token t)
{
    (void) data; (void) datalen;
    int err = 0;
    int response = 0;
    int cfun;
    char *line;
    ATResponse *atresponse;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &atresponse);
    if (err < 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &cfun);
    if (err < 0)
        goto error;

    assert(cfun >= 0 && cfun < 7);

    switch (cfun) {
    case 5:
        response = 1;
        break;
    case 6:
        response = 2;
        break;
    case 1:
  response = 3;
  break;
    default:
        response = 0;
        break;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE
 *
 * Query current network selectin mode.
 */
void requestQueryNetworkSelectionMode(void *data, size_t datalen,
                                      RIL_Token t)
{
    (void) data; (void) datalen;
    int err;
    ATResponse *atresponse = NULL;
    int response = 0;
    char *line;

    err = at_send_command_singleline("AT+COPS?", "+COPS:", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);

    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &response);

    if (err < 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &response, sizeof(int));

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("requestQueryNetworkSelectionMode must never return error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SIGNAL_STRENGTH
 *
 * Requests current signal strength and bit error rate.
 *
 * Must succeed if radio is on.
 */
void requestSignalStrength(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    ATResponse *atresponse = NULL;
    int err;
    RIL_SignalStrength signalStrength;
    char *line;
    int ber;
    int rssi;

    memset(&signalStrength, 0, sizeof(RIL_SignalStrength));

    err = at_send_command_singleline("AT+CSQ", "+CSQ:", &atresponse);

    if (err < 0 || atresponse->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        goto error;
    }

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line,&rssi);
    if (err < 0)
        goto error;
    signalStrength.GW_SignalStrength.signalStrength = rssi;

    err = at_tok_nextint(&line, &ber);
    if (err < 0)
        goto error;
    signalStrength.GW_SignalStrength.bitErrorRate = ber;

    at_response_free(atresponse);
    atresponse = NULL;
    /*
     * If we get 99 as signal strenght, we are probably on WDCMA. Try 
     * AT+CIND to give some indication on what signal strength we got.
     *
     * Android calculates rssi and dBm values from this value, so the dBm
     * value presented in android will be wrong, but this is an error on
     * android's end.
     */
    if (rssi == 99) {
        err = at_send_command_singleline("AT+CIND?", "+CIND:", &atresponse);
        if (err < 0 || atresponse->success == 0)
            goto error;

        line = atresponse->p_intermediates->line;

        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line,
                             &signalStrength.GW_SignalStrength.signalStrength);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line,
                             &signalStrength.GW_SignalStrength.signalStrength);
        if (err < 0)
            goto error;

        if (signalStrength.GW_SignalStrength.signalStrength > 0) {
            signalStrength.GW_SignalStrength.signalStrength *= 4;
            signalStrength.GW_SignalStrength.signalStrength--;
        }
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &signalStrength,
                          sizeof(RIL_SignalStrength));

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("requestSignalStrength must never return an error when radio is on");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * Convert detailedReason from modem to what Android expects.
 * Called in requestRegistrationState().
 */
static
Reg_Deny_DetailReason convertRegistrationDeniedReason(int detailedReason)
{
    Reg_Deny_DetailReason reason;

    switch (detailedReason) {
    case 3:
        reason = NETWORK_FAILURE;
        break;
    case 8:
        reason = PLMN_NOT_ALLOWED;
        break;
    case 9:
        reason = LOCATION_AREA_NOT_ALLOWED;
        break;
    case 10:
        reason = ROAMING_NOT_ALLOWED;
        break;
    case 12:
        reason = NO_SUITABLE_CELL_IN_LOCATION_AREA;
        break;
    case 13:
        reason = AUTHENTICATION_FAILURE;
        break;
    case 16:
        reason = IMSI_UNKNOWN_IN_HLR;
        break;
    case 17:
        reason = ILLEGAL_MS;
        break;
    case 18:
        reason = ILLEGAL_ME;
        break;
    default:
        reason = GENERAL;
        break;
    }

    return reason;
}

/**
 * RIL_REQUEST_GPRS_REGISTRATION_STATE
 *
 * Request current GPRS registration state.
 */
void requestGprsRegistrationState(int request, void *data,
                              size_t datalen, RIL_Token t)
{
    (void)request, (void)data, (void)datalen;
    int err = 0;
    const char resp_size = 4;
    int response[resp_size];
    char *responseStr[resp_size];
    ATResponse *atresponse = NULL, *p_response = NULL;
    char *line, *p;
    int commas = 0;
    int skip, tmp;
    int count = 3;
    int ul_sp = 0;
    int dl_sp = 0;
    int gsm_rinfo = 0;
    int umts_rinfo = 0;

    getScreenStateLock();
    if (!getScreenState())
        (void)at_send_command("AT+CGREG=2", NULL); /* Response not vital */

    memset(responseStr, 0, sizeof(responseStr));
    memset(response, 0, sizeof(response));
    response[1] = -1;
    response[2] = -1;

    err = at_send_command_singleline("AT+CGREG?", "+CGREG: ", &atresponse);
    if (err < 0 || atresponse->success == 0 ||
        atresponse->p_intermediates == NULL) {
        goto error;
    }

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;
    /*
     * The solicited version of the +CGREG response is
     * +CGREG: n, stat, [lac, cid [,<AcT>]]
     * and the unsolicited version is
     * +CGREG: stat, [lac, cid [,<AcT>]]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be.
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both.
     *
     * Also since the LAC, CID and AcT are only reported when registered,
     * we can have 1, 2, 3, 4 or 5 arguments here.
     */
    /* Count number of commas */
    p = line;
    err = at_tok_charcounter(line, ',', &commas);
    if (err < 0) {
        LOGE("at_tok_charcounter failed.\r\n");
        goto error;
    }

    switch (commas) {
    case 0:                    /* +CGREG: <stat> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        break;

    case 1:                    /* +CGREG: <n>, <stat> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        break;

    case 2:                    /* +CGREG: <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[1]);
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[2]);
        if (err < 0) goto error;
        break;

    case 3:                    /* +CGREG: <n>, <stat>, <lac>, <cid> */
                               /* +CGREG: <stat>, <lac>, <cid>, <AcT> */
        err = at_tok_nextint(&line, &tmp);
        if (err < 0) goto error;

        /* We need to check if the second parameter is <lac> */
        if (*(line) == '"') {
            response[0] = tmp; /* <stat> */
            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
            if (err < 0) goto error;
            err = at_tok_nextint(&line, &response[3]); /* <AcT> */
            if (err < 0) goto error;
            count = 4;
        } else {
            err = at_tok_nextint(&line, &response[0]); /* <stat> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
            if (err < 0) goto error;
            err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
            if (err < 0) goto error;
        }
        break;

    case 4:                    /* +CGREG: <n>, <stat>, <lac>, <cid>, <AcT> */
        err = at_tok_nextint(&line, &skip); /* <n> */
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[0]); /* <stat> */
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[1]); /* <lac> */
        if (err < 0) goto error;
        err = at_tok_nexthexint(&line, &response[2]); /* <cid> */
        if (err < 0) goto error;
        err = at_tok_nextint(&line, &response[3]); /* <AcT> */
        if (err < 0) goto error;
        count = 4;
        break;

    default:
        LOGE("Invalid input.\r\n");
        goto error;
    }
    if (response[0] == CGREG_STAT_REG_HOME_NET ||
        response[0] == CGREG_STAT_ROAMING) {
            err = at_send_command_singleline("AT*ERINFO?", "*ERINFO:",
                                             &p_response);
            if (err < 0 || p_response->success == 0)
                    goto error;

            line = p_response->p_intermediates->line;
            err = at_tok_start(&line);
            if (err < 0)
                    goto finally;

            err = at_tok_nextint(&line, &skip);
            if (err < 0)
                    goto finally;

            err = at_tok_nextint(&line, &gsm_rinfo);
            if (err < 0)
                    goto finally;

            err = at_tok_nextint(&line, &umts_rinfo);
            if (err < 0)
                    goto finally;

            if (umts_rinfo > ERINFO_UMTS_NO_UMTS_HSDPA)
                    response[3] = CGREG_ACT_UTRAN;

            /*
             *      0 == unknown
             *      1 == GPRS only
             *      2 == EDGE
             *      3 == UMTS
             *      9 == HSDPA
             *      11 == HSPA
             */
            /* +CGEQNEG: 1,3,384,7296,16,64,0,1500,"1E4","4E3",0,1000,2 */
            /* Sometime this command fails to respond */
            if (getE2napState() == E2NAP_ST_CONNECTED) {
                    err =
                            at_send_command_singleline("AT+CGEQNEG", "+CGEQNEG:",
                                            &p_response);
                    if (err < 0 || p_response->success == 0)
                            goto error;

                    line = p_response->p_intermediates->line;
                    err = at_tok_start(&line);
                    if (err < 0)
                            goto finally;

                    err = at_tok_nextint(&line, &skip);
                    if (err < 0)
                            goto finally;

                    err = at_tok_nextint(&line, &skip);
                    if (err < 0)
                            goto finally;

                    err = at_tok_nextint(&line, &ul_sp);
                    if (err < 0)
                            goto finally;

                    err = at_tok_nextint(&line, &dl_sp);
                    if (err < 0)
                            goto finally;

                    LOGD("Max speed %i/%i, UL/DL", ul_sp, dl_sp);
            }

            if (umts_rinfo >= ERINFO_UMTS_HSDPA) {
                    if (ul_sp > 384)
                            response[3] = CGREG_ACT_UTRAN_HSUPA_HSDPA;
                    else
                            response[3] = CGREG_ACT_UTRAN_HSDPA;
            }
    }

    /* Converting to stringlist for Android */
    asprintf(&responseStr[0], "%d", response[0]); /* state */

    if (response[1] >= 0)
        asprintf(&responseStr[1], "%04x", response[1]); /* LAC */
    else
        responseStr[1] = NULL;

    if (response[2] >= 0)
        asprintf(&responseStr[2], "%08x", response[2]); /* CID */
    else
        responseStr[2] = NULL;

    if (count > 3) {
        /*
         * Android expects something like this here:
         *
         *    0 == unknown
         *    1 == GPRS only
         *    2 == EDGE
         *    3 == UMTS
         *    9 == HSDPA
         *    10 == HSUPA
         *    11 == HSPA
         *
         * +CGREG response:
         *    0 GSM
         *    1 GSM Compact (Not Supported)
         *    2 UTRAN
         *    3 GSM w/EGPRS
         *    4 UTRAN w/HSDPA
         *    5 UTRAN w/HSUPA
         *    6 UTRAN w/HSUPA and HSDPA
         */
        int networkType;

        /* Converstion between AT AcT and Android NetworkType */
        switch (response[3]) {
        case CGREG_ACT_GSM:
            networkType = 1;
            break;
        case CGREG_ACT_UTRAN:
            networkType = 3;
            break;
        case CGREG_ACT_GSM_EGPRS:
            networkType = 2;
            break;
        case CGREG_ACT_UTRAN_HSDPA:
            networkType = 9;
            break;
        case CGREG_ACT_UTRAN_HSUPA:
            networkType = 10;
            break;
        case CGREG_ACT_UTRAN_HSUPA_HSDPA:
            networkType = 11;
            break;
        default:
            networkType = 0;
            break;
        }
        /* available radio technology */
        asprintf(&responseStr[3], "%d", networkType);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr, resp_size * sizeof(char *));

finally:
    if (!getScreenState())
        (void)at_send_command("AT+CGREG=0", NULL);

    releaseScreenStateLock(); /* Important! */

    if (responseStr[0])
        free(responseStr[0]);
    if (responseStr[1])
        free(responseStr[1]);
    if (responseStr[2])
        free(responseStr[2]);
    if (responseStr[3])
        free(responseStr[3]);

    at_response_free(atresponse);
    at_response_free(p_response);
    return;

error:
    LOGE("requestRegistrationState must never return an error when radio is "
         "on.");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_GPRS_REGISTRATION_STATE
 *
 * Request current GPRS registration state.
 */
void requestRegistrationState(int request, void *data,
                              size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen, (void)request;
    int err = 0;
    const char resp_size = 15;
    int response[resp_size];
    char *responseStr[resp_size];
    ATResponse *cgreg_resp = NULL, *e2reg_resp = NULL;
    char *line, *p;
    int commas = 0;
    int skip, cs_status = 0;
    int i;
    int count = 3;

    /* IMPORTANT: Will take screen state lock here. Make sure to always call
                  releaseScreenStateLock BEFORE returning! */
    getScreenStateLock();
    if (!getScreenState()) {
        (void)at_send_command("AT+CREG=2", NULL); /* Ignore the response, not VITAL. */
    }

    /* Setting default values in case values are not returned by AT command */
    for (i = 0; i < resp_size; i++)
        responseStr[i] = NULL;

    memset(response, 0, sizeof(response));

    err = at_send_command_singleline("AT+CREG?", "+CREG:", &cgreg_resp);

    if (err < 0 ||
        cgreg_resp->success == 0 ||
        cgreg_resp->p_intermediates == NULL) {
        goto error;
    }

    line = cgreg_resp->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0) {
        goto error;
    }

    /* 
     * The solicited version of the CREG response is
     * +CREG: n, stat, [lac, cid]
     * and the unsolicited version is
     * +CREG: stat, [lac, cid]
     * The <n> parameter is basically "is unsolicited creg on?"
     * which it should always be.
     *
     * Now we should normally get the solicited version here,
     * but the unsolicited version could have snuck in
     * so we have to handle both.
     *
     * Also since the LAC and CID are only reported when registered,
     * we can have 1, 2, 3, or 4 arguments here.
     *
     * finally, a +CGREG: answer may have a fifth value that corresponds
     * to the network type, as in;
     *
     *   +CGREG: n, stat [,lac, cid [,networkType]]
     */

    /* Count number of commas */
    for (p = line; *p != '\0'; p++) {
        if (*p == ',')
            commas++;
    }

    switch (commas) {
    case 0:                    /* +CREG: <stat> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0)
            goto error;

        response[1] = -1;
        response[2] = -1;
        break;

    case 1:                    /* +CREG: <n>, <stat> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response[0]);
        if (err < 0)
            goto error;

        response[1] = -1;
        response[2] = -1;
        if (err < 0)
            goto error;
        break;
    case 2:                    /* +CREG: <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &response[0]);
        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[1]);
        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[2]);
        if (err < 0)
            goto error;
        break;
    case 3:                    /* +CREG: <n>, <stat>, <lac>, <cid> */
        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &response[0]);
        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[1]);
        if (err < 0)
            goto error;

        err = at_tok_nexthexint(&line, &response[2]);
        if (err < 0)
            goto error;
        break;
    default:
        goto error;
    }

    if (response[0] == CGREG_STAT_REG_DENIED) {
        err = at_send_command_singleline("AT*E2REG?", "*E2REG:",
                                         &e2reg_resp);

        if (err < 0 || e2reg_resp->success == 0)
            goto error;

        line = e2reg_resp->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &skip);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &cs_status);
        if (err < 0)
            goto error;

        response[13] = convertRegistrationDeniedReason(cs_status);
        s_registrationDeniedReason = response[13];
        asprintf(&responseStr[13], "%08x", response[13]);
    }

    s_registrationDeniedReason = DEFAULT_VALUE;

    /* This was incorrect in the reference implementation. Go figure. FIXME */
    asprintf(&responseStr[0], "%d", response[0]);

    if (response[1] > 0)
        asprintf(&responseStr[1], "%04x", response[1]);

    if (response[2] > 0)
        asprintf(&responseStr[2], "%08x", response[2]);

    RIL_onRequestComplete(t, RIL_E_SUCCESS, responseStr,
                          count * sizeof(char *));

finally:
    if (!getScreenState())
        (void)at_send_command("AT+CREG=0", NULL);

    releaseScreenStateLock(); /* Important! */

    for (i = 0; i < resp_size; i++)
        free(responseStr[i]);

    at_response_free(cgreg_resp);
    at_response_free(e2reg_resp);
    return;

error:
    LOGE("requestRegistrationState must never return an error when radio is on.");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_OPERATOR
 *
 * Request current operator ONS or EONS.
 */
void requestOperator(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    int err;
    int i;
    int skip;
    ATLine *cursor;
    char *response[3];
    ATResponse *atresponse = NULL;

    memset(response, 0, sizeof(response));

    err = at_send_command_multiline
        ("AT+COPS=3,0;+COPS?;+COPS=3,1;+COPS?;+COPS=3,2;+COPS?", "+COPS:",
         &atresponse);

    /* We expect 3 lines here:
     * +COPS: 0,0,"T - Mobile"
     * +COPS: 0,1,"TMO"
     * +COPS: 0,2,"310170"
     */

    if (err < 0)
        goto error;

    for (i = 0, cursor = atresponse->p_intermediates; cursor != NULL;
         cursor = cursor->p_next, i++) {
        char *line = cursor->line;

        err = at_tok_start(&line);

        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &skip);

        if (err < 0)
            goto error;

        /* If we're unregistered, we may just get
           a "+COPS: 0" response. */
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextint(&line, &skip);

        if (err < 0)
            goto error;

        /* A "+COPS: 0, n" response is also possible. */
        if (!at_tok_hasmore(&line)) {
            response[i] = NULL;
            continue;
        }

        err = at_tok_nextstr(&line, &(response[i]));

        if (err < 0)
            goto error;
    }

    if (i != 3)
        goto error;

    /* 
     * Check if modem returned an empty string, and fill it with MNC/MMC 
     * if that's the case.
     */
    if (response[0] && strlen(response[0]) == 0) {
        response[0] = alloca(strlen(response[2]) + 1);
        strcpy(response[0], response[2]);
    }

    if (response[1] && strlen(response[1]) == 0) {
        response[1] = alloca(strlen(response[2]) + 1);
        strcpy(response[1], response[2]);
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, response, sizeof(response));

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

/**
 * RIL_REQUEST_SET_LOCATION_UPDATES
 *
 * Enables/disables network state change notifications due to changes in
 * LAC and/or CID (basically, +CREG=2 vs. +CREG=1).  
 *
 * Note:  The RIL implementation should default to "updates enabled"
 * when the screen is on and "updates disabled" when the screen is off.
 *
 * See also: RIL_REQUEST_SCREEN_STATE, RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED.
 */
void requestSetLocationUpdates(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int enable = 0;
    int err = 0;
    char *cmd;
    ATResponse *atresponse = NULL;

    enable = ((int *) data)[0];
    assert(enable == 0 || enable == 1);

    asprintf(&cmd, "AT+CREG=%d", (enable == 0 ? 1 : 2));
    err = at_send_command(cmd, &atresponse);
    free(cmd);

    if (err < 0 || atresponse->success == 0)
        goto error;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}
