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

#include <telephony/ril.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "atchannel.h"
#include "at_tok.h"
#include "fcp_parser.h"
#include "u300-ril.h"
#include "u300-ril-sim.h"
#include "misc.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

typedef enum {
    SIM_ABSENT = 0,
    SIM_NOT_READY = 1,
    SIM_READY = 2,         /* SIM_READY means the radio state is RADIO_STATE_SIM_READY. */
    SIM_PIN = 3,
    SIM_PUK = 4,
    SIM_NETWORK_PERSONALIZATION = 5
} SIM_Status;

typedef enum {
    UICC_TYPE_UNKNOWN,
    UICC_TYPE_SIM,
    UICC_TYPE_USIM,
} UICC_Type;

static const struct timeval TIMEVAL_SIMPOLL = { 1, 0 };
static const struct timeval TIMEVAL_SIMRESET = { 60, 0 };
static int sim_hotswap;

int get_pending_hotswap()
{
    return sim_hotswap;
}

void set_pending_hotswap(int pending_hotswap)
{
    sim_hotswap = pending_hotswap;
}

static void resetSim(void *param)
{
    (void) param;
    ATResponse *atresponse = NULL;
    int err, state;
    char *line;

    err = at_send_command_singleline("AT*ESIMSR?", "*ESIMSR:", &atresponse);
    if (err < 0 || atresponse->success == 0)
        goto error;

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    if (state == 7) {
        at_send_command("AT*ESIMR", NULL);

        enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, resetSim, NULL,
                        &TIMEVAL_SIMRESET);
    } else {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
                                  NULL, 0);
        pollSIMState(NULL);
    }

finally:
    at_response_free(atresponse);
    return;

error:
    goto finally;
}

void onSimStateChanged(const char *s)
{
    int err, state;
    char *tok;
    char *line = tok = strdup(s);

    RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED, NULL, 0);

    /* Also check sim state, that will trigger radio state to sim absent. */
    enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollSIMState, (void *) 1, NULL);

    /* 
     * Now, find out if we went to poweroff-state. If so, enqueue some loop
     * to try to reset the SIM for a minute or so to try to recover.
     */
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    if (state == 7) {
        enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, resetSim, NULL, NULL);
    }

finally:
    free(tok);
    return;

error:
    LOGE("ERROR in onSimStateChanged!");
    goto finally;
}

void onSimHotswap(const char *s)
{
    if (strcmp ("*EESIMSWAP:0", s) == 0) {
        LOGD("SIM REMOVED");
        setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
    } else if (strcmp ("*EESIMSWAP:1", s) == 0) {
        LOGD("SIM INSERTED");
        set_pending_hotswap(1);
    } else
        LOGD("UNKNOWN HOT SWAP EVENT: %s", s);
}

/** Returns one of SIM_*. Returns SIM_NOT_READY on error. */
static SIM_Status getSIMStatus()
{
    ATResponse *atresponse = NULL;
    int err;
    int ret;
    char *cpinLine;
    char *cpinResult;

    if (currentState() == RADIO_STATE_OFF ||
        currentState() == RADIO_STATE_UNAVAILABLE) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_send_command_singleline("AT+CPIN?", "+CPIN:", &atresponse);

    if (err != 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    switch (at_get_cme_error(atresponse)) {
    case CME_SUCCESS:
        break;

    case CME_SIM_NOT_INSERTED:
        ret = SIM_ABSENT;
        goto done;

    case CME_SIM_FAILURE:
        ret = SIM_ABSENT;
        goto done;

    default:
        ret = SIM_NOT_READY;
        goto done;
    }

    /* CPIN? has succeeded, now look at the result. */

    cpinLine = atresponse->p_intermediates->line;
    err = at_tok_start(&cpinLine);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    err = at_tok_nextstr(&cpinLine, &cpinResult);

    if (err < 0) {
        ret = SIM_NOT_READY;
        goto done;
    }

    if (0 == strcmp(cpinResult, "SIM PIN")) {
        ret = SIM_PIN;
        goto done;
    } else if (0 == strcmp(cpinResult, "SIM PUK")) {
        ret = SIM_PUK;
        goto done;
    } else if (0 == strcmp(cpinResult, "PH-NET PIN")) {
        ret = SIM_NETWORK_PERSONALIZATION;
	goto done;
    } else if (0 != strcmp(cpinResult, "READY")) {
        /* We're treating unsupported lock types as "sim absent". */
        ret = SIM_ABSENT;
        goto done;
    }

    ret = SIM_READY;

done:
    at_response_free(atresponse);
    return ret;
}

/**
 * Fetch information about UICC card type (SIM/USIM)
 *
 * \return UICC_Type: type of UICC card.
 */
static UICC_Type getUICCType()
{
    ATResponse *atresponse = NULL;
    static UICC_Type UiccType = UICC_TYPE_UNKNOWN;
    int err;

    if (currentState() == RADIO_STATE_OFF ||
        currentState() == RADIO_STATE_UNAVAILABLE) {
        return UICC_TYPE_UNKNOWN;
    }

    if (UiccType == UICC_TYPE_UNKNOWN) {
        err = at_send_command_singleline("AT+CUAD", "+CUAD:", &atresponse);
        if (err == 0 && atresponse->success) {
            /* USIM */
            UiccType = UICC_TYPE_USIM;
            LOGI("Detected card type USIM - stored");
        } else if (err == 0 && !atresponse->success) {
            /* Command failed - unknown card */
            UiccType = UICC_TYPE_UNKNOWN;
            LOGE("getUICCType(): Failed to detect card type - Retry at next request");
        } else {
            /* Legacy SIM */
            /* TODO: CUAD only responds OK if SIM is inserted.
             *       This is an inccorect AT response...
             */
            UiccType = UICC_TYPE_SIM;
            LOGI("Detected card type Legacy SIM - stored");
        }
        at_response_free(atresponse);
    }

    return UiccType;
}


/**
 * Get the current card status.
 *
 * This must be freed using freeCardStatus.
 * @return: On success returns RIL_E_SUCCESS.
 */
static int getCardStatus(RIL_CardStatus **pp_card_status) {
    static RIL_AppStatus app_status_array[] = {
        /* SIM_ABSENT = 0 */
        { RIL_APPTYPE_UNKNOWN, RIL_APPSTATE_UNKNOWN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        /* SIM_NOT_READY = 1 */
        { RIL_APPTYPE_SIM, RIL_APPSTATE_DETECTED, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        /* SIM_READY = 2 */
        { RIL_APPTYPE_SIM, RIL_APPSTATE_READY, RIL_PERSOSUBSTATE_READY,
          NULL, NULL, 0, RIL_PINSTATE_UNKNOWN, RIL_PINSTATE_UNKNOWN },
        /* SIM_PIN = 3 */
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PIN, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN },
        /* SIM_PUK = 4 */
        { RIL_APPTYPE_SIM, RIL_APPSTATE_PUK, RIL_PERSOSUBSTATE_UNKNOWN,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_BLOCKED, RIL_PINSTATE_UNKNOWN },
        /* SIM_NETWORK_PERSONALIZATION = 5 */
        { RIL_APPTYPE_SIM, RIL_APPSTATE_SUBSCRIPTION_PERSO, RIL_PERSOSUBSTATE_SIM_NETWORK,
          NULL, NULL, 0, RIL_PINSTATE_ENABLED_NOT_VERIFIED, RIL_PINSTATE_UNKNOWN }
    };
    RIL_CardState card_state;
    int num_apps;

    SIM_Status sim_status = getSIMStatus();
    if (sim_status == SIM_ABSENT) {
        card_state = RIL_CARDSTATE_ABSENT;
        num_apps = 0;
    } else {
        card_state = RIL_CARDSTATE_PRESENT;
        num_apps = 1;
    }

    /* Allocate and initialize base card status. */
    RIL_CardStatus *p_card_status = malloc(sizeof(RIL_CardStatus));
    p_card_status->card_state = card_state;
    p_card_status->universal_pin_state = RIL_PINSTATE_UNKNOWN;
    p_card_status->gsm_umts_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->cdma_subscription_app_index = RIL_CARD_MAX_APPS;
    p_card_status->num_applications = num_apps;

    /* Initialize application status. */
    int i;
    for (i = 0; i < RIL_CARD_MAX_APPS; i++) {
        p_card_status->applications[i] = app_status_array[SIM_ABSENT];
    }

    /* Pickup the appropriate application status
       that reflects sim_status for gsm. */
    if (num_apps != 0) {
        UICC_Type uicc_type = getUICCType();

        /* Only support one app, gsm/wcdma. */
        p_card_status->num_applications = 1;
        p_card_status->gsm_umts_subscription_app_index = 0;

        /* Get the correct app status. */
        p_card_status->applications[0] = app_status_array[sim_status];
        if (uicc_type == UICC_TYPE_SIM) {
            LOGI("[Card type discovery]: Legacy SIM");
        } else { /* defaulting to USIM */
            LOGI("[Card type discovery]: USIM");
            p_card_status->applications[0].app_type = RIL_APPTYPE_USIM;
        }
    }

    *pp_card_status = p_card_status;
    return RIL_E_SUCCESS;
}

/**
 * Free the card status returned by getCardStatus.
 */
static void freeCardStatus(RIL_CardStatus *p_card_status) {
    free(p_card_status);
}

/**
 * SIM ready means any commands that access the SIM will work, including:
 *  AT+CPIN, AT+CSMS, AT+CNMI, AT+CRSM
 *  (all SMS-related commands).
 */
void pollSIMState(void *param)
{
    if (((int) param) != 1 &&
        currentState() != RADIO_STATE_SIM_NOT_READY &&
        currentState() != RADIO_STATE_SIM_LOCKED_OR_ABSENT) {
        /* No longer valid to poll. */
        return;
    }

    switch (getSIMStatus()) {
    case SIM_ABSENT:
    case SIM_PIN:
    case SIM_PUK:
    case SIM_NETWORK_PERSONALIZATION:
    default:
        setRadioState(RADIO_STATE_SIM_LOCKED_OR_ABSENT);
        return;

    case SIM_NOT_READY:
        enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollSIMState, NULL,
                        &TIMEVAL_SIMPOLL);
        return;

    case SIM_READY:
        setRadioState(RADIO_STATE_SIM_READY);
        return;
    }
}

/**
 * Get the number of retries left for pin functions
 */
static int getNumRetries (int request) {
    ATResponse *atresponse = NULL;
    int err;
    char *cmd = NULL;
    int num_retries = -1;

    asprintf(&cmd, "AT*EPIN?");
    err = at_send_command_singleline(cmd, "*EPIN:", &atresponse);
    free(cmd);

    switch (request) {
    case RIL_REQUEST_ENTER_SIM_PIN:
    case RIL_REQUEST_CHANGE_SIM_PIN:
        sscanf(atresponse->p_intermediates->line, "*EPIN: %d",
               &num_retries);
        break;
    case RIL_REQUEST_ENTER_SIM_PUK:
        sscanf(atresponse->p_intermediates->line, "*EPIN: %*d,%d",
               &num_retries);
        break;
    case RIL_REQUEST_ENTER_SIM_PIN2:
    case RIL_REQUEST_CHANGE_SIM_PIN2:
        sscanf(atresponse->p_intermediates->line, "*EPIN: %*d,%*d,%d",
               &num_retries);
        break;
    case RIL_REQUEST_ENTER_SIM_PUK2:
        sscanf(atresponse->p_intermediates->line, "*EPIN: %*d,%*d,%*d,%d",
               &num_retries);
        break;
    default:
        num_retries = -1;
    break;
    }

    at_response_free(atresponse);
    return num_retries;
}

/** 
 * RIL_REQUEST_GET_SIM_STATUS
 *
 * Requests status of the SIM interface and the SIM card.
 * 
 * Valid errors:
 *  Must never fail.
 */
void requestGetSimStatus(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    RIL_CardStatus* p_card_status = NULL;

    if (getCardStatus(&p_card_status) != RIL_E_SUCCESS)
        goto error;
    
    RIL_onRequestComplete(t, RIL_E_SUCCESS, (char*)p_card_status, sizeof(*p_card_status));

finally:
    if (p_card_status != NULL) {
        freeCardStatus(p_card_status);
    }
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

static int simIOGetLogicalChannel()
{
    ATResponse *atresponse = NULL;
    static int g_lc = 0;
    char *cmd = NULL;
    int err;

    if (g_lc == 0) {
        struct tlv tlvApp, tlvAppId;
        char *line;
        char *resp;

        err = at_send_command_singleline("AT+CUAD", "+CUAD:", &atresponse);
        if (err < 0)
            goto error;
        if (atresponse->success == 0) {
            err = -EINVAL;
            goto error;
        }

        line = atresponse->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextstr(&line, &resp);
        if (err < 0)
            goto error;

        err = parseTlv(resp, &resp[strlen(resp)], &tlvApp);
        if (err < 0)
            goto error;
        if (tlvApp.tag != 0x61) { /* Application */
            err = -EINVAL;
            goto error;
        }

        err = parseTlv(tlvApp.data, tlvApp.end, &tlvAppId);
        if (err < 0)
            goto error;
        if (tlvAppId.tag != 0x4F) { /* Application ID */
            err = -EINVAL;
            goto error;
        }

        asprintf(&cmd, "AT+CCHO=\"%.*s\"",
            tlvAppId.end - tlvAppId.data, tlvAppId.data);
        if (cmd == NULL) {
            err = -ENOMEM;
            goto error;
        }

        at_response_free(atresponse);
        err = at_send_command_singleline(cmd, "+CCHO:", &atresponse);
        if (err < 0)
            goto error;

        if (atresponse->success == 0) {
            err = -EINVAL;
            goto error;
        }

        line = atresponse->p_intermediates->line;
        err = at_tok_start(&line);
        if (err < 0)
            goto error;

        err = at_tok_nextint(&line, &g_lc);
        if (err < 0)
            goto error;
    }

finally:
    at_response_free(atresponse);
    free(cmd);
    return g_lc;

error:
    goto finally;
}

static int simIOSelectFile(unsigned short fileid)
{
    int err = 0;
    char *cmd = NULL;
    unsigned short lc = simIOGetLogicalChannel();
    ATResponse *atresponse = NULL;
    char *line;
    char *resp;
    int resplen;

    if (lc == 0) {
        err = -EIO;
        goto error;
    }

    asprintf(&cmd, "AT+CGLA=%d,14,\"00A4000C02%.4X\"",
        lc, fileid);
    if (cmd == NULL) {
        err = -ENOMEM;
        goto error;
    }

    err = at_send_command_singleline(cmd, "+CGLA:", &atresponse);
    if (err < 0)
        goto error;
    if (atresponse->success == 0) {
        err = -EINVAL;
        goto error;
    }

    line = atresponse->p_intermediates->line;
    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &resplen);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &resp);
    if (err < 0)
        goto error;

    /* Std resp code: "9000" */
    if (resplen != 4 || strcmp(resp, "9000") != 0) {
        err = -EIO;
        goto error;
    }

finally:
    at_response_free(atresponse);
    free(cmd);
    return err;

error:
    goto finally;
}

static int simIOSelectPath(const char *path, unsigned short fileid)
{
    int err = 0;
    size_t path_len = 0;
    size_t pos;
    static char cashed_path[4 * 10 + 1] = {0};
    static unsigned short cashed_fileid = 0;

    if (path == NULL) {
        path = "3F00";
    }
    path_len = strlen(path);

    if (path_len & 3) {
        err = -EINVAL;
        goto error;
    }

    if ((fileid != cashed_fileid) || (strcmp(path, cashed_path) != 0)) {
        for(pos = 0; pos < path_len; pos += 4) {
            unsigned val;
            if(sscanf(&path[pos], "%4X", &val) != 1) {
                err = -EINVAL;
                goto error;
            }
            err = simIOSelectFile(val);
            if (err < 0)
                goto error;
        }
        err = simIOSelectFile(fileid);
    }
    if (path_len < sizeof(cashed_path)) {
        strcpy(cashed_path, path);
        cashed_fileid = fileid;
    } else {
        cashed_path[0] = 0;
        cashed_fileid = 0;
    }

finally:
    return err;

error:
    goto finally;
}

int sendSimIOCmdUICC(const RIL_SIM_IO *ioargs, ATResponse *atresponse, RIL_SIM_IO_Response *sr)
{
    int err;
    int resplen;
    char *line, *resp;
    char *cmd = NULL, *data = NULL;
    unsigned short lc = simIOGetLogicalChannel();
    unsigned char sw1, sw2;

    if (lc == 0) {
        err = -EIO;
        goto error;
    }

    memset(sr, 0, sizeof(*sr));

    switch (ioargs->command) {
        case 0xC0: /* Get response */
            /* Convert Get response to Select. */
            asprintf(&data, "00A4000402%.4X00",
                ioargs->fileid);
            break;

        case 0xB0: /* Read binary */
        case 0xB2: /* Read record */
            asprintf(&data, "00%.2X%.2X%.2X%.2X",
                (unsigned char)ioargs->command,
                (unsigned char)ioargs->p1,
                (unsigned char)ioargs->p2,
                (unsigned char)ioargs->p3);
            break;

        case 0xD6: /* Update binary */
        case 0xDC: /* Update record */
            if (!ioargs->data) {
                err = -EINVAL;
                goto error;
            }
            asprintf(&data, "00%.2X%.2X%.2X%.2X%s",
                (unsigned char)ioargs->command,
                (unsigned char)ioargs->p1,
                (unsigned char)ioargs->p2,
                (unsigned char)ioargs->p3,
                ioargs->data);
            break;

        default:
            err = -ENOTSUP;
            goto error;
    }
    if (data == NULL) {
        err = -ENOMEM;
        goto error;
    }

    asprintf(&cmd, "AT+CGLA=%d,%d,\"%s\"", lc, strlen(data), data);
    if (cmd == NULL) {
        err = -ENOMEM;
        goto error;
    }

    err = simIOSelectPath(ioargs->path, ioargs->fileid);
    if (err < 0)
        goto error;

    err = at_send_command_singleline(cmd, "+CGLA:", &atresponse);
    if (err < 0)
        goto error;

    if (atresponse->success == 0) {
        err = -EINVAL;
        goto error;
    }

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &resplen);
    if (err < 0)
        goto error;

    err = at_tok_nextstr(&line, &resp);
    if (err < 0)
        goto error;

    if ((resplen < 4) || ((size_t)resplen != strlen(resp))) {
        err = -EINVAL;
        goto error;
    }

    err = stringToBinary(&resp[resplen - 4], 2, &sw1);
    if (err < 0)
        goto error;

    err = stringToBinary(&resp[resplen - 2], 2, &sw2);
    if (err < 0)
        goto error;

    sr->sw1 = sw1;
    sr->sw2 = sw2;
    resp[resplen - 4] = 0;
    sr->simResponse = resp;

finally:
    free(cmd);
    free(data);
    return err;

error:
    goto finally;

}


int sendSimIOCmdICC(const RIL_SIM_IO *ioargs, ATResponse *atresponse, RIL_SIM_IO_Response *sr)
{
    int err;
    char *cmd = NULL;
    char *fmt;
    char *arg6;
    char *arg7;
    char *line;

    /* FIXME Handle pin2. */
    memset(sr, 0, sizeof(*sr));

    arg6 = ioargs->data;
    arg7 = ioargs->path;

    if (arg7 && arg6) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,\"%s\",\"%s\"";
    } else if (arg7) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,,\"%s\"";
        arg6 = arg7;
    } else if (arg6) {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d,\"%s\"";
    } else {
        fmt = "AT+CRSM=%d,%d,%d,%d,%d";
    }

    asprintf(&cmd, fmt,
             ioargs->command, ioargs->fileid,
             ioargs->p1, ioargs->p2, ioargs->p3,
             arg6, arg7);

    if (cmd == NULL) {
        err = -ENOMEM;
        goto error;
    }

    err = at_send_command_singleline(cmd, "+CRSM:", &atresponse);
    if (err < 0)
        goto error;

    if (atresponse->success == 0) {
        err = -EINVAL;
        goto error;
    }

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(sr->sw1));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(sr->sw2));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) {
        err = at_tok_nextstr(&line, &(sr->simResponse));
        if (err < 0)
            goto error;
    }

finally:
    free(cmd);
    return err;

error:
    goto finally;
}

int sendSimIOCmd(const RIL_SIM_IO *ioargs, ATResponse *atresponse, RIL_SIM_IO_Response *sr)
{
    int err;
    UICC_Type UiccType;

    if (sr == NULL)
        return -1;

    /* Detect card type to determine which SIM access command to use */
    UiccType = getUICCType();

    /*
     * FIXME WORKAROUND: Currently GCLA works from some files on some cards
     * and CRSM works on some files for some cards...
     * Trying with CRSM first and retry with CGLA if needed
     */
    err = sendSimIOCmdICC(ioargs, atresponse, sr);
    if ((err < 0 || (sr->sw1 != 0x90 && sr->sw2 != 0x00)) &&
            UiccType != UICC_TYPE_SIM) {
        at_response_free(atresponse);
        LOGD("sendSimIOCmd(): Retrying with CGLA access...");
        err = sendSimIOCmdUICC(ioargs, atresponse, sr);
    }
    /* END WORKAROUND */

    /* reintroduce below code when workaround is not needed */
    /* if (UiccType == UICC_TYPE_SIM)
        err = sendSimIOCmdICC(ioargs, atresponse, sr);
    else {
        err = sendSimIOCmdUICC(ioargs, atresponse, sr);
    } */

    return err;
}

int convertSimIoFcp(RIL_SIM_IO_Response *sr, char **cvt)
{
    int err;
    /* size_t pos; */
    size_t fcplen;
    struct ts_51011_921_resp resp;
    void *cvt_buf = NULL;

    if (!sr->simResponse || !cvt) {
        err = -EINVAL;
        goto error;
    }

    fcplen = strlen(sr->simResponse);
    if ((fcplen == 0) || (fcplen & 1)) {
        err = -EINVAL;
        goto error;
    }

    err = fcp_to_ts_51011(sr->simResponse, fcplen, &resp);
    if (err < 0)
        goto error;

    cvt_buf = malloc(sizeof(resp) * 2 + 1);
    if (!cvt_buf) {
        err = -ENOMEM;
        goto error;
    }

    err = binaryToString((unsigned char*)(&resp),
                   sizeof(resp), cvt_buf);
    if (err < 0)
        goto error;

    /* cvt_buf ownership is moved to the caller */
    *cvt = cvt_buf;
    cvt_buf = NULL;

finally:
    return err;

error:
    free(cvt_buf);
    goto finally;
}


/**
 * RIL_REQUEST_SIM_IO
 *
 * Request SIM I/O operation.
 * This is similar to the TS 27.007 "restricted SIM" operation
 * where it assumes all of the EF selection will be done by the
 * callee.
 */
void requestSIM_IO(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    ATResponse *atresponse = NULL;
    RIL_SIM_IO_Response sr;
    RIL_SIM_IO *ioargs;
    int cvt_done = 0;
    int err;
    UICC_Type UiccType = getUICCType();

    memset(&sr, 0, sizeof(sr));

    ioargs = (RIL_SIM_IO *) data;

    err = sendSimIOCmd(ioargs, atresponse, &sr);
    if (err < 0)
        goto error;

    /*
     * In case the command is GET_RESPONSE and cardtype is 3G SIM
     * convert to 2G FCP
     */
    if (ioargs->command == 0xC0 && UiccType != UICC_TYPE_SIM) {
        err = convertSimIoFcp(&sr, &sr.simResponse);
        if (err < 0)
            goto error;
        cvt_done = 1; /* sr.simResponse needs to be freed */
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &sr, sizeof(sr));

finally:
    at_response_free(atresponse);
    if (cvt_done)
        free(sr.simResponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;

}

/**
 * Enter SIM PIN, might be PIN, PIN2, PUK, PUK2, etc.
 *
 * Data can hold pointers to one or two strings, depending on what we
 * want to enter. (PUK requires new PIN, etc.).
 *
 * FIXME: Do we need to return remaining tries left on error as well?
 *        Also applies to the rest of the requests that got the retries
 *        in later commits to ril.h.
 */
void requestEnterSimPin(void *data, size_t datalen, RIL_Token t, int request)
{
    ATResponse *atresponse = NULL;
    int err;
    int cme_err;
    char *cmd = NULL;
    const char **strings = (const char **) data;
    int num_retries = -1;

    if (datalen == sizeof(char *)) {
        asprintf(&cmd, "AT+CPIN=\"%s\"", strings[0]);
    } else if (datalen == 2 * sizeof(char *)) {
        asprintf(&cmd, "AT+CPIN=\"%s\",\"%s\"", strings[0], strings[1]);
    } else
        goto error;

    err = at_send_command(cmd, &atresponse);
    free(cmd);

    cme_err = at_get_cme_error(atresponse);

    if (cme_err != CME_SUCCESS && (err < 0 || atresponse->success == 0)) {
        if (cme_err == 16 || cme_err == 17 || cme_err == 18 || cme_err == 11 || cme_err == 12) {
            num_retries = getNumRetries (request);
            RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &num_retries, sizeof(int *));
        } else
            goto error;

    } else {
        /*
         * Got OK, return success and wait for *EPEV to trigger poll
         * of SIM state.
         */

        /* TODO: Check if we can get number of retries remaining. */
        num_retries = getNumRetries (request);
        RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));
    }

finally:
    at_response_free(atresponse);
    return;
error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

void requestChangePassword(void *data, size_t datalen, RIL_Token t,
                           char *facility, int request)
{
    int err = 0;
    char *oldPassword = NULL;
    char *newPassword = NULL;
    char *cmd = NULL;
    ATResponse *atresponse = NULL;
    int num_retries = -1;

    if (datalen != 2 * sizeof(char *) || strlen(facility) != 2)
        goto error;


    oldPassword = ((char **) data)[0];
    newPassword = ((char **) data)[1];

    asprintf(&cmd, "AT+CPWD=\"%s\",\"%s\",\"%s\"", facility, oldPassword,
             newPassword);

    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0)
        goto error;

    num_retries = getNumRetries(request);
    RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));

finally:
    at_response_free(atresponse);
    return;

error:
    if (atresponse != NULL && at_get_cme_error(atresponse) == 16) {
        num_retries = getNumRetries(request);
        RIL_onRequestComplete(t, RIL_E_PASSWORD_INCORRECT, &num_retries, sizeof(int *));
    } else {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    }
    goto finally;
}

/**
 * RIL_REQUEST_SET_FACILITY_LOCK
 *
 * Enable/disable one facility lock.
 */
void requestSetFacilityLock(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int err;
    ATResponse *atresponse = NULL;
    char *cmd = NULL;
    char *facility_string = NULL;
    int facility_mode = -1;
    char *facility_mode_str = NULL;
    char *facility_password = NULL;
    char *facility_class = NULL;
    int num_retries = -1;

    assert(datalen >= (4 * sizeof(char **)));

    facility_string = ((char **) data)[0];
    facility_mode_str = ((char **) data)[1];
    facility_password = ((char **) data)[2];
    facility_class = ((char **) data)[3];

    assert(*facility_mode_str == '0' || *facility_mode_str == '1');
    facility_mode = atoi(facility_mode_str);


    asprintf(&cmd, "AT+CLCK=\"%s\",%d,\"%s\",%s", facility_string,
             facility_mode, facility_password, facility_class);
    err = at_send_command(cmd, &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0) {
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, &num_retries, sizeof(int *));
    at_response_free(atresponse);
    return;

error:
    at_response_free(atresponse);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
}

/**
 * RIL_REQUEST_QUERY_FACILITY_LOCK
 *
 * Query the status of a facility lock state.
 */
void requestQueryFacilityLock(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int err, response;
    /* int rat; */
    ATResponse *atresponse = NULL;
    char *cmd = NULL;
    char *line = NULL;
    char *facility_string = NULL;
    char *facility_password = NULL;
    char *facility_class = NULL;

    assert(datalen >= (3 * sizeof(char **)));

    facility_string = ((char **) data)[0];
    facility_password = ((char **) data)[1];
    facility_class = ((char **) data)[2];

    asprintf(&cmd, "AT+CLCK=\"%s\",2,\"%s\",%s", facility_string,
             facility_password, facility_class);
    err = at_send_command_singleline(cmd, "+CLCK:", &atresponse);
    free(cmd);
    if (err < 0 || atresponse->success == 0) {
        goto error;
    }

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
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}
