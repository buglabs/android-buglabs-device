/* ST-Ericsson U300 RIL
 *
 * Copyright (C) ST-Ericsson AB 2008-2009
 * Copyright 2006, The Android Open Source Project
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
 * Based on reference-ril by The Android Open Source Project.
 *
 * Heavily modified for ST-Ericsson U300 modems.
 * Author: Christian Bejram <christian.bejram@stericsson.com>
 */

#include <telephony/ril.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <alloca.h>
#include <getopt.h>
#include <sys/socket.h>
#include <cutils/sockets.h>
#include <termios.h>
#include <stdbool.h>

#include "atchannel.h"
#include "at_tok.h"
#include "misc.h"

#include "u300-ril.h"
#include "u300-ril-config.h"
#include "u300-ril-messaging.h"
#include "u300-ril-network.h"
#include "u300-ril-pdp.h"
#include "u300-ril-services.h"
#include "u300-ril-sim.h"
#include "u300-ril-oem.h"
#include "u300-ril-requestdatahandler.h"
#include "u300-ril-error.h"

#define LOG_TAG "RIL"
#include <utils/Log.h>

#define RIL_VERSION_STRING "MBM u300-ril 1.9.3"

#define MAX_AT_RESPONSE 0x1000

#define timespec_cmp(a, b, op)         \
        ((a).tv_sec == (b).tv_sec    \
        ? (a).tv_nsec op (b).tv_nsec \
        : (a).tv_sec op (b).tv_sec)

static void onConnectionStateChanged(const char *s);

/*** Declarations ***/
static void onRequest(int request, void *data, size_t datalen,
                      RIL_Token t);
static int onSupports(int requestCode);
static void onCancel(RIL_Token t);
static const char *getVersion(void);
static int isRadioOn();
static void signalCloseQueues(void);
extern const char *requestToString(int request);

/*** Static Variables ***/
static const RIL_RadioFunctions s_callbacks = {
    RIL_VERSION,
    onRequest,
    currentState,
    onSupports,
    onCancel,
    getVersion
};

static RIL_RadioState sState = RADIO_STATE_UNAVAILABLE;

/*TODO: fix this bad this can dead lock?!?!?*/
static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_screen_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_e2nap_mutex = PTHREAD_MUTEX_INITIALIZER;

static int s_screenState = true;

static int s_e2napState = -1;
static int s_e2napCause = -1;

typedef struct RILRequest {
    int request;
    void *data;
    size_t datalen;
    RIL_Token token;
    struct RILRequest *next;
} RILRequest;

typedef struct RILEvent {
    void (*eventCallback) (void *param);
    void *param;
    struct timespec abstime;
    struct RILEvent *next;
    struct RILEvent *prev;
} RILEvent;

typedef struct RequestQueue {
    pthread_mutex_t queueMutex;
    pthread_cond_t cond;
    RILRequest *requestList;
    RILEvent *eventList;
    char enabled;
    char closed;
} RequestQueue;

static RequestQueue s_requestQueue = {
    .queueMutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .requestList = NULL,
    .eventList = NULL,
    .enabled = 1,
    .closed = 1
};

static RequestQueue s_requestQueuePrio = {
    .queueMutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .requestList = NULL,
    .eventList = NULL,
    .enabled = 0,
    .closed = 1
};

static RequestQueue *s_requestQueues[] = {
    &s_requestQueue,
    &s_requestQueuePrio
};

static const struct timeval TIMEVAL_0 = { 0, 0 };

/**
 * Enqueue a RILEvent to the request queue. isPrio specifies in what queue
 * the request will end up.
 *
 * 0 = the "normal" queue, 1 = prio queue and 2 = both. If only one queue
 * is present, then the event will be inserted into that queue.
 */
void enqueueRILEvent(int isPrio, void (*callback) (void *param), 
                     void *param, const struct timeval *relativeTime)
{
    struct timeval tv;
    char done = 0;
    RequestQueue *q = NULL;

    RILEvent *e = malloc(sizeof(RILEvent));
    memset(e, 0, sizeof(RILEvent));

    e->eventCallback = callback;
    e->param = param;

    if (relativeTime == NULL) {
        relativeTime = alloca(sizeof(struct timeval));
        memset((struct timeval *) relativeTime, 0, sizeof(struct timeval));
    }
    
    gettimeofday(&tv, NULL);

    e->abstime.tv_sec = tv.tv_sec + relativeTime->tv_sec;
    e->abstime.tv_nsec = (tv.tv_usec + relativeTime->tv_usec) * 1000;

    if (e->abstime.tv_nsec > 1000000000) {
        e->abstime.tv_sec++;
        e->abstime.tv_nsec -= 1000000000;
    }

    if (!s_requestQueuePrio.enabled || 
        (isPrio == RIL_EVENT_QUEUE_NORMAL || isPrio == RIL_EVENT_QUEUE_ALL)) {
        q = &s_requestQueue;
    } else if (isPrio == RIL_EVENT_QUEUE_PRIO) {
        q = &s_requestQueuePrio;
    }

again:
    pthread_mutex_lock(&q->queueMutex);

    if (q->eventList == NULL) {
        q->eventList = e;
    } else {
        if (timespec_cmp(q->eventList->abstime, e->abstime, > )) {
            e->next = q->eventList;
            q->eventList->prev = e;
            q->eventList = e;
        } else {
            RILEvent *tmp = q->eventList;
            do {
                if (timespec_cmp(tmp->abstime, e->abstime, > )) {
                    tmp->prev->next = e;
                    e->prev = tmp->prev;
                    tmp->prev = e;
                    e->next = tmp;
                    break;
                } else if (tmp->next == NULL) {
                    tmp->next = e;
                    e->prev = tmp;
                    break;
                }
                tmp = tmp->next;
            } while (tmp);
        }
    }
    
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->queueMutex);

    if (s_requestQueuePrio.enabled && isPrio == RIL_EVENT_QUEUE_ALL && !done) {
        RILEvent *e2 = malloc(sizeof(RILEvent));
        memcpy(e2, e, sizeof(RILEvent));
        e = e2;
        done = 1;
        q = &s_requestQueuePrio;

        goto again;
    }

    return;
}

/** Do post-AT+CFUN=1 initialization. */
static void onRadioPowerOn()
{
    enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollSIMState, NULL, NULL);
}

/** Do post- SIM ready initialization. */
static void onSIMReady()
{
    ATResponse *atresponse = NULL;
    int err = 0;

    /* Select message service */
    at_send_command("AT+CSMS=0", NULL);

   /* Configure new messages indication 
    *  mode = 2 - Buffer unsolicited result code in TA when TA-TE link is 
    *             reserved(e.g. in on.line data mode) and flush them to the 
    *             TE after reservation. Otherwise forward them directly to 
    *             the TE. 
    *  mt   = 2 - SMS-DELIVERs (except class 2 messages and messages in the 
    *             message waiting indication group (store message)) are 
    *             routed directly to TE using unsolicited result code:
    *             +CMT: [<alpha>],<length><CR><LF><pdu> (PDU mode)
    *             Class 2 messages are handled as if <mt> = 1
    *  bm   = 2 - New CBMs are routed directly to the TE using unsolicited
    *             result code:
    *             +CBM: <length><CR><LF><pdu> (PDU mode)
    *  ds   = 1 - SMS-STATUS-REPORTs are routed to the TE using unsolicited
    *             result code: +CDS: <length><CR><LF><pdu> (PDU mode)
    *  dfr  = 0 - TA buffer of unsolicited result codes defined within this
    *             command is flushed to the TE when <mode> 1...3 is entered
    *             (OK response is given before flushing the codes).
    */
    at_send_command("AT+CNMI=2,2,2,1,0", NULL);

    /* Configure preferred message storage 
     *   mem1 = SM, mem2 = SM, mem3 = SM
     */
    at_send_command("AT+CPMS=\"SM\",\"SM\",\"SM\"", NULL);

    /* Subscribe to network registration events. 
     *  n = 2 - Enable network registration and location information 
     *          unsolicited result code +CREG: <stat>[,<lac>,<ci>] 
     */
    err = at_send_command("AT+CREG=2", &atresponse);
    if (err < 0 || atresponse->success == 0) {
        /* Some handsets -- in tethered mode -- don't support CREG=2. */
        at_send_command("AT+CREG=1", NULL);
    }
    at_response_free(atresponse);
    atresponse = NULL;

    /* Don't subscribe to Ericsson network registration events
     *  n = 0 - Disable network registration unsolicited result codes.
     */
    at_send_command("AT*EREG=0", NULL);

    /* Subsctibe to Call Waiting Notifications.
     *  n = 1 - Enable call waiting notifications
     */
    at_send_command("AT+CCWA=1", NULL);

    /* Configure mute control.
     *  n 0 - Mute off
     */
    at_send_command("AT+CMUT=0", NULL);

    /* Subscribe to Supplementary Services Notification
     *  n = 1 - Enable the +CSSI result code presentation status.
     *          Intermediaate result codes. When enabled and a supplementary
     *          service notification is received after a mobile originated
     *          call setup.
     *  m = 1 - Enable the +CSSU result code presentation status.
     *          Unsolicited result code. When a supplementary service 
     *          notification is received during a mobile terminated call
     *          setup or during a call, or when a forward check supplementary
     *          service notification is received.
     */
    at_send_command("AT+CSSN=1,1", NULL);

    /* Subscribe to Unstuctured Supplementary Service Data (USSD) notifications.
     *  n = 1 - Enable result code presentation in the TA.
     */
    at_send_command("AT+CUSD=1", NULL);

    /* Subscribe to Packet Domain Event Reporting.
     *  mode = 1 - Discard unsolicited result codes when ME-TE link is reserved
     *             (e.g. in on-line data mode); otherwise forward them directly
     *             to the TE.
     *   bfr = 0 - MT buffer of unsolicited result codes defined within this
     *             command is cleared when <mode> 1 is entered.
     */
    at_send_command("AT+CGEREP=1,0", NULL);

    /* Configure Short Message (SMS) Format 
     *  mode = 0 - PDU mode.
     */
    at_send_command("AT+CMGF=0", NULL);

    /* Subscribe to ST-Ericsson time zone/NITZ reporting.
     *  
     */
    at_send_command("AT*ETZR=2", NULL);

    /* Subscribe to ST-Ericsson Call monitoring events.
     *  onoff = 1 - Call monitoring is on
     */
    at_send_command("AT*ECAM=1", NULL);

    /* SIM Application Toolkit Configuration
     *  n = 1 - Enable SAT unsolicited result codes
     *  stkPrfl = - SIM application toolkit profile in hexadecimal format 
     *              starting with first byte of the profile.
     *              See 3GPP TS 11.14[1] for details.
     */
    /* TODO: Investigate if we need to set profile, or if it's ignored as
             described in the AT specification. */
    at_send_command("AT*STKC=1,\"000000000000000000\"", NULL);

    /* Configure Mobile Equipment Event Reporting.
     *  mode = 3 - Forward unsolicited result codes directly to the TE;
     *             There is no inband technique used to embed result codes
     *             and data when TA is in on-line data mode.
     */
    at_send_command("AT+CMER=3,0,0,1", NULL);
}

/**
 * RIL_REQUEST_GET_IMSI
*/
static void requestGetIMSI(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command_numeric("AT+CIMI", &atresponse);

    if (err < 0 || atresponse->success == 0)
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    else
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
                              atresponse->p_intermediates->line,
                              sizeof(char *));

    at_response_free(atresponse);
    return;
}

/* RIL_REQUEST_DEVICE_IDENTITY
 *
 * Request the device ESN / MEID / IMEI / IMEISV.
 *
 */
static void requestDeviceIdentity(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    ATResponse *atresponse = NULL;
    char* response[4];
    int err;
    char* svn;

    /* IMEI */ 
    err = at_send_command_numeric("AT+CGSN", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;
    
    response[0] = atresponse->p_intermediates->line;

    /* IMEISV */
    at_response_free(atresponse);
    atresponse = NULL;
    err = at_send_command_multiline("AT*EVERS", "SVN", &atresponse);

    if (err < 0 || atresponse->success == 0) {
        at_response_free(atresponse);
        atresponse = NULL;
        err = at_send_command_multiline("AT*EEVINFO", "SVN", &atresponse);

        if (err < 0 || atresponse->success == 0)
            goto error;
    }

    svn = malloc(strlen(atresponse->p_intermediates->line));
    if (!svn)
        goto error;

    sscanf(atresponse->p_intermediates->line, "SVN%*s %s", svn);
    response[1] = svn;

    /* CDMA not supported */
    response[2] = "";
    response[3] = "";

    RIL_onRequestComplete(t, RIL_E_SUCCESS,
                          &response,
                          sizeof(response));

    free(svn);

    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(atresponse);
}

/* Deprecated */
/**
 * RIL_REQUEST_GET_IMEI
 *
 * Get the device IMEI, including check digit.
*/
static void requestGetIMEI(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    ATResponse *atresponse = NULL;
    int err;

    err = at_send_command_numeric("AT+CGSN", &atresponse);

    if (err < 0 || atresponse->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    } else {
        RIL_onRequestComplete(t, RIL_E_SUCCESS,
                              atresponse->p_intermediates->line,
                              sizeof(char *));
    }
    at_response_free(atresponse);
    return;
}

/* Deprecated */
/**
 * RIL_REQUEST_GET_IMEISV
 *
 * Get the device IMEISV, which should be two decimal digits.
*/
static void requestGetIMEISV(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    char *response = NULL;

    ATResponse *atresponse = NULL;
    int err;
    char svn[5];

    /* IMEISV */
    atresponse = NULL;
    err = at_send_command_multiline("AT*EVERS", "SVN", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto eevinfo;
    else
        goto parse_svn;

error:
    at_response_free(atresponse);
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    return;

eevinfo:
    at_response_free(atresponse);
    atresponse = NULL;
    err = at_send_command_multiline("AT*EEVINFO", "SVN", &atresponse);

    if (err < 0 || atresponse->success == 0)
        goto error;

parse_svn:
    sscanf(atresponse->p_intermediates->line, "SVN%*s %s", svn);
    asprintf(&response, "%s", svn);

    at_response_free(atresponse);

    RIL_onRequestComplete(t, RIL_E_SUCCESS,
                          response,
                          sizeof(char *));

    if (response)
        free(response);
}

/**
 * RIL_REQUEST_RADIO_POWER
 *
 * Toggle radio on and off (for "airplane" mode).
*/
static void requestRadioPower(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int onOff;
    int err;
    ATResponse *atresponse = NULL;

    assert(datalen >= sizeof(int *));
    onOff = ((int *) data)[0];

    if (onOff == 0 && sState != RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=4", &atresponse);
        if (err < 0 || atresponse->success == 0)
            goto error;
        setRadioState(RADIO_STATE_OFF);
    } else if (onOff > 0 && sState == RADIO_STATE_OFF) {
        err = at_send_command("AT+CFUN=1", &atresponse);
        if (err < 0 || atresponse->success == 0) {
            goto error;
        }
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    } else {
        LOGE("Erroneous input to requestRadioPower()!");
        goto error;
    }
    
    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

    at_response_free(atresponse);
    return;

error:
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    at_response_free(atresponse);
    return;
}

int getE2napState()
{
    return s_e2napState;
}

int getE2napCause()
{
    return s_e2napCause;
}

int setE2napState(int state)
{
    s_e2napState = state;
    return s_e2napState;
}

int setE2napCause(int state)
{
    s_e2napCause = state;
    return s_e2napCause;
}

/**
 * Will LOCK THE MUTEX! MAKE SURE TO RELEASE IT!
 */
void getScreenStateLock(void)
{
    /* Just make sure we're not changing anything with regards to screen state. */
    pthread_mutex_lock(&s_screen_state_mutex);
}

int getScreenState(void)
{
    return s_screenState;
}

void setScreenState(bool screenIsOn)
{
    s_screenState = screenIsOn;
}

void releaseScreenStateLock(void)
{
    pthread_mutex_unlock(&s_screen_state_mutex);
}

static void requestScreenState(void *data, size_t datalen, RIL_Token t)
{
    (void) datalen;
    int err, screenState;

    assert(datalen >= sizeof(int *));

    pthread_mutex_lock(&s_screen_state_mutex);
    screenState = s_screenState = ((int *) data)[0];

    if (screenState == 1) {
        /* Screen is on - be sure to enable all unsolicited notifications again. */
        err = at_send_command("AT+CREG=2", NULL);
        if (err < 0)
            goto error;
        err = at_send_command("AT+CGREG=2", NULL);
        if (err < 0)
            goto error;
        err = at_send_command("AT+CGEREP=1,0", NULL);
        if (err < 0)
            goto error;
        if (at_send_command("AT+CMER=3,0,0,1", NULL) < 0)
            goto error;
    } else if (screenState == 0) {
        /* Screen is off - disable all unsolicited notifications. */
        err = at_send_command("AT+CREG=0", NULL);
        if (err < 0)
            goto error;
        err = at_send_command("AT+CGREG=0", NULL);
        if (err < 0)
            goto error;
        err = at_send_command("AT+CGEREP=0,0", NULL);
        if (err < 0)
            goto error;
        if (err < 0)
            goto error;
    } else {
        /* Not a defined value - error. */
        goto error;
    }

    RIL_onRequestComplete(t, RIL_E_SUCCESS, NULL, 0);

finally:
    pthread_mutex_unlock(&s_screen_state_mutex);
    return;

error:
    LOGE("ERROR: requestScreenState failed");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);

    releaseScreenStateLock();
    goto finally;
}

enum clccNoCLI {
    CLCC_NOCLI_NOT_SET = -1,
    CLCC_NOCLI_UNKNOWN = 0,
    CLCC_NOCLI_RESTRICTED = 1,
    CLCC_NOCLI_OTHER_SERVICE = 2,
    CLCC_NOCLI_PAYPHONE = 3
};

enum clccState {
    CLCC_STATE_ACTIVE = 0,
    CLCC_STATE_HELD = 1,
    CLCC_STATE_DIALING = 2,
    CLCC_STATE_ALERTING = 3,
    CLCC_STATE_INCOMMING = 4,
    CLCC_STATE_WAITING = 5
};

static int clccStateToRILState(int state, RIL_CallState *p_state)
{
    switch (state) {
    case CLCC_STATE_ACTIVE:
        *p_state = RIL_CALL_ACTIVE;
        return 0;
    case CLCC_STATE_HELD:
        *p_state = RIL_CALL_HOLDING;
        return 0;
    case CLCC_STATE_DIALING:
        *p_state = RIL_CALL_DIALING;
        return 0;
    case CLCC_STATE_ALERTING:
        *p_state = RIL_CALL_ALERTING;
        return 0;
    case CLCC_STATE_INCOMMING:
        *p_state = RIL_CALL_INCOMING;
        return 0;
    case CLCC_STATE_WAITING:
        *p_state = RIL_CALL_WAITING;
        return 0;
    default:
        return -1;
    }
}

static int clccCauseNoCLIToRILPres(int clccCauseNoCLI, int *rilPresentation)
{
    /*
     * Converting clccCauseNoCLI to RIL_numberPresentation
     * AT+CLCC cause_no_CLI     <> RIL number/name presentation
     * --------------------------------------------------------
     *-1=Parameter non existant <> 0=Allowed
     * 0=Unknown                <> 2=Not Specified/Unknown
     * 1=Restricted             <> 1=Restricted
     * 2=Other service          <> 2=Not Specified/Unknown
     * 3=Payphone               <> 3=Payphone
     *
     * Note that CLCC does not include cause of no CLI if
     * presentation is allowed, hence the -1 to 0 mapping.
     */
    switch (clccCauseNoCLI) {
    case CLCC_NOCLI_NOT_SET:
        *rilPresentation = 0;
        return 0;
    case CLCC_NOCLI_UNKNOWN:
        *rilPresentation = 2;
        return 0;
    case CLCC_NOCLI_RESTRICTED:
    case CLCC_NOCLI_OTHER_SERVICE:
    case CLCC_NOCLI_PAYPHONE:
        *rilPresentation = clccCauseNoCLI;
        return 0;
    default: /* clccCauseNoCLIval -> unknown cause */
        *rilPresentation = 2;
        return -1;
    }
}

/**
 * Note: Directly modified line and has *p_call point directly into
 * modified line.
 * Returns -1bha if failed to decode line, 0 on success.
 */
static int callFromCLCCLine(char *line, RIL_Call *p_call)
{
    /*
     * +CLCC: index,isMT,state,mode,isMpty(,number,type(,alpha(,priority(,cause_of_no_cli))))
     * example of individual values +CLCC: 1,0,2,0,0,"+15161218005",145,"Hansen",0,1
     */
    int err;
    int state;
    int mode;
    int priority;
    int causeNoCLI = -1;
    int success = 0;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &(p_call->index));
    if (err < 0)
        goto error;

    err = at_tok_nextbool(&line, &(p_call->isMT));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &state);
    if (err < 0)
        goto error;

    err = clccStateToRILState(state, &(p_call->state));
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &mode);
    if (err < 0)
        goto error;

    p_call->isVoice = (mode == 0);

    err = at_tok_nextbool(&line, &(p_call->isMpty));
    if (err < 0)
        goto error;

    if (at_tok_hasmore(&line)) { /* optional number and toa */
        err = at_tok_nextstr(&line, &(p_call->number)); /* accepting empty string */
        err = at_tok_nextint(&line, &p_call->toa);
        if (err < 0 && (p_call->number != NULL && strlen(p_call->number) > 0))
            goto error;

        if (at_tok_hasmore(&line)) { /* optional alphanummeric name */
            err = at_tok_nextstr(&line, &(p_call->name)); /* accepting empty string */

            if (at_tok_hasmore(&line)) { /* optional priority */
                err = at_tok_nextint(&line, &priority); /* accepting empty string */

                if (at_tok_hasmore(&line)) { /* optional cause_no_CLI */
                    err = at_tok_nextint(&line, &causeNoCLI);
                    if (err < 0)
                        goto error;
                }
            }
        }
    }

    /*
     * Converting cause of no CLI:
     * Note: CLCC does not include cause of no CLI if number/name is presented;
     *       -1 indicates this to the conversion function.
     */
    err = clccCauseNoCLIToRILPres(causeNoCLI, &p_call->numberPresentation);
    if (err < 0)
        LOGI("CLCC: cause of no CLI contained an unknown number, update required?");

    /*
     * Modem may not have support for cause of no CLI, if number is
     * non-existant we cannot assume we really got cause of no CLI.
     * Checking and resetting to unknown if not supported.
     */
    if ((p_call->number == NULL || strlen(p_call->number) == 0) &&
        p_call->numberPresentation == 0)
        p_call->numberPresentation = 2;

    /*
     * Cause is mainly related to Number. Name comes from phonebook in modem.
     * Based on Name availability set namePresentation.
     */
    if ((p_call->name == NULL || strlen(p_call->name) == 0) &&
        p_call->numberPresentation == 0)
        p_call->namePresentation = 2;
    else
        p_call->namePresentation = p_call->numberPresentation;

finally:
    return success;

error:
    LOGE("%s: invalid CLCC line\n", __func__);
    success = -1;
    goto finally;
}

/**
 * RIL_REQUEST_GET_CURRENT_CALLS
 *
 * Requests current call list.
 */
void requestGetCurrentCalls(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    int err;
    ATResponse *atresponse = NULL;
    ATLine *cursor;
    int countCalls;
    int countValidCalls = 0;
    RIL_Call *calls = NULL;
    RIL_Call **response = NULL;
    RIL_Call *call = NULL;
    int i;

    err = at_send_command_multiline("AT+CLCC", "+CLCC:", &atresponse);

    if (err != 0 || atresponse->success == 0) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* Count the calls. */
    for (countCalls = 0, cursor = atresponse->p_intermediates;
         cursor != NULL; cursor = cursor->p_next)
        countCalls++;

    if (countCalls == 0)
        goto exit;

    /* Yes, there's an array of pointers and then an array of structures. */
    response = (RIL_Call **) alloca(countCalls * sizeof(RIL_Call *));
    calls = (RIL_Call *) alloca(countCalls * sizeof(RIL_Call));

    /* Init the pointer array. */
    for (i = 0; i < countCalls; i++)
        response[i] = &(calls[i]);

    for (countValidCalls = 0, cursor = atresponse->p_intermediates;
         cursor != NULL; cursor = cursor->p_next) {
        call = calls + countValidCalls;
        memset(call, 0, sizeof(*call));
        call->number  = NULL;
        call->name    = NULL;
        call->uusInfo = NULL;

        err = callFromCLCCLine(cursor->line, call);
        if (err != 0)
            continue;

        countValidCalls++;
    }

exit:
    RIL_onRequestComplete(t, RIL_E_SUCCESS, response,
                          countValidCalls * sizeof(RIL_Call *));

    at_response_free(atresponse);
    return;
}

/**
 * RIL_REQUEST_BASEBAND_VERSION
 *
 * Return string value indicating baseband version, eg
 * response from AT+CGMR.
*/
static void requestBasebandVersion(void *data, size_t datalen, RIL_Token t)
{
    (void) data; (void) datalen;
    int err;
    ATResponse *atresponse = NULL;
    char *line;

    err = at_send_command_singleline("AT+CGMR", "\0", &atresponse);

    if (err < 0 || 
        atresponse->success == 0 || 
        atresponse->p_intermediates == NULL) {
        goto error;
    }

    line = atresponse->p_intermediates->line;

    RIL_onRequestComplete(t, RIL_E_SUCCESS, line, sizeof(char *));

finally:
    at_response_free(atresponse);
    return;

error:
    LOGE("Error in requestBasebandVersion()");
    RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
    goto finally;
}

static char isPrioRequest(int request)
{
    unsigned int i;
    for (i = 0; i < sizeof(prioRequests) / sizeof(int); i++)
        if (request == prioRequests[i])
            return 1;
    return 0;
}

static void processRequest(int request, void *data, size_t datalen, RIL_Token t)
{
    LOGE("processRequest: %s", requestToString(request));

    /* Ignore all requests except RIL_REQUEST_GET_SIM_STATUS
     * when RADIO_STATE_UNAVAILABLE.
     */
    if (sState == RADIO_STATE_UNAVAILABLE
        && request != RIL_REQUEST_GET_SIM_STATUS) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }

    /* Ignore all non-power requests when RADIO_STATE_OFF
     * (except RIL_REQUEST_GET_SIM_STATUS and a few more).
     */
    if ((sState == RADIO_STATE_OFF || sState == RADIO_STATE_SIM_NOT_READY)
        && !(request == RIL_REQUEST_RADIO_POWER || 
             request == RIL_REQUEST_GET_SIM_STATUS ||
             request == RIL_REQUEST_GET_IMEISV ||
             request == RIL_REQUEST_GET_IMEI ||
             request == RIL_REQUEST_BASEBAND_VERSION ||
             request == RIL_REQUEST_SCREEN_STATE)) {
        RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
        return;
    }
    
    /* 
     * These commands won't accept RADIO_NOT_AVAILABLE, so we just return
     * GENERIC_FAILURE if we're not in SIM_STATE_READY.
     */
    if (sState != RADIO_STATE_SIM_READY
        && (request == RIL_REQUEST_WRITE_SMS_TO_SIM ||
            request == RIL_REQUEST_DELETE_SMS_ON_SIM)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    /* Don't allow radio operations when sim is absent or locked! */
    if (sState == RADIO_STATE_SIM_LOCKED_OR_ABSENT
        && !(request == RIL_REQUEST_ENTER_SIM_PIN ||
             request == RIL_REQUEST_ENTER_SIM_PUK ||
             request == RIL_REQUEST_ENTER_SIM_PIN2 ||
             request == RIL_REQUEST_ENTER_SIM_PUK2 ||
             request == RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION ||
             request == RIL_REQUEST_GET_SIM_STATUS ||
             request == RIL_REQUEST_RADIO_POWER ||
             request == RIL_REQUEST_GET_IMEISV ||
             request == RIL_REQUEST_GET_IMEI ||
             request == RIL_REQUEST_BASEBAND_VERSION ||
             request == RIL_REQUEST_GET_CURRENT_CALLS)) {
        RIL_onRequestComplete(t, RIL_E_GENERIC_FAILURE, NULL, 0);
        return;
    }

    switch (request) {

	
        case RIL_REQUEST_GET_CURRENT_CALLS:
            if (sState == RADIO_STATE_SIM_LOCKED_OR_ABSENT)
                RIL_onRequestComplete(t, RIL_E_RADIO_NOT_AVAILABLE, NULL, 0);
            else
                requestGetCurrentCalls(data, datalen, t);
            break;
        case RIL_REQUEST_SCREEN_STATE:
            requestScreenState(data, datalen, t);
            /* Trigger a rehash of network values, just to be sure. */
            if (((int *)data)[0] == 1)
                RIL_onUnsolicitedResponse(
                                   RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
                                   NULL, 0);
            break;

        /* Data Call Requests */
        case RIL_REQUEST_SETUP_DATA_CALL:
            requestSetupDefaultPDP(data, datalen, t);
            break;
        case RIL_REQUEST_DEACTIVATE_DATA_CALL:
            requestDeactivateDefaultPDP(data, datalen, t);
            break;
        case RIL_REQUEST_LAST_DATA_CALL_FAIL_CAUSE:
            requestLastPDPFailCause(data, datalen, t);
            break;
        case RIL_REQUEST_DATA_CALL_LIST:
            requestPDPContextList(data, datalen, t);
            break;

        /* SMS Requests */
        case RIL_REQUEST_SEND_SMS:
            requestSendSMS(data, datalen, t);
            break;
        /* case RIL_REQUEST_SEND_SMS_EXPECT_MORE:
            requestSendSMSExpectMore(data, datalen, t);
            break;
        case RIL_REQUEST_WRITE_SMS_TO_SIM:
            requestWriteSmsToSim(data, datalen, t);
            break;
        case RIL_REQUEST_DELETE_SMS_ON_SIM:
            requestDeleteSmsOnSim(data, datalen, t);
            break; */
        case RIL_REQUEST_GET_SMSC_ADDRESS:
            requestGetSMSCAddress(data, datalen, t);
            break;
        case RIL_REQUEST_SET_SMSC_ADDRESS:
            requestSetSMSCAddress(data, datalen, t);
            break;
         case RIL_REQUEST_SMS_ACKNOWLEDGE:
            requestSMSAcknowledge(data, datalen, t);
            break;
        case RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG:
            requestGSMGetBroadcastSMSConfig(data, datalen, t);
            break;
        case RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG:
            requestGSMSetBroadcastSMSConfig(data, datalen, t);
            break;
        case RIL_REQUEST_GSM_SMS_BROADCAST_ACTIVATION:
            requestGSMSMSBroadcastActivation(data, datalen, t);
            break;

        /* SIM Handling Requests */
        case RIL_REQUEST_SIM_IO:
            requestSIM_IO(data, datalen, t);
            break;
        case RIL_REQUEST_GET_SIM_STATUS:
            requestGetSimStatus(data, datalen, t);
            break;
        case RIL_REQUEST_ENTER_SIM_PIN:
        case RIL_REQUEST_ENTER_SIM_PUK:
        case RIL_REQUEST_ENTER_SIM_PIN2:
        case RIL_REQUEST_ENTER_SIM_PUK2:
            requestEnterSimPin(data, datalen, t, request);
            break;
        case RIL_REQUEST_CHANGE_SIM_PIN:
            requestChangePassword(data, datalen, t, "SC", request);
            break;
        case RIL_REQUEST_CHANGE_SIM_PIN2:
            requestChangePassword(data, datalen, t, "P2", request);
            break;
        /* case RIL_REQUEST_CHANGE_BARRING_PASSWORD:
            requestChangePassword(data + sizeof(char *), 
                                  datalen - sizeof(char *), t, 
                                  ((char**) data)[0], request);
            break; */
        case RIL_REQUEST_QUERY_FACILITY_LOCK:
            requestQueryFacilityLock(data, datalen, t);
            break;
        case RIL_REQUEST_SET_FACILITY_LOCK:
            requestSetFacilityLock(data, datalen, t);
            break;

        /* USSD Requests */
        /* case RIL_REQUEST_SEND_USSD:
            requestSendUSSD(data, datalen, t);
            break;
        case RIL_REQUEST_CANCEL_USSD:
            requestCancelUSSD(data, datalen, t);
            break; */

        /* Network Selection */
        /* case RIL_REQUEST_SET_BAND_MODE:
            requestSetBandMode(data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE:
            requestQueryAvailableBandMode(data, datalen, t);
            break; */
        case RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION:
            requestEnterSimPin(data, datalen, t, request);
            break;
        case RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE:
            requestQueryNetworkSelectionMode(data, datalen, t);
            break;
        case RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC:
            requestSetNetworkSelectionAutomatic(data, datalen, t);
            break;
        case RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL:
            requestSetNetworkSelectionManual(data, datalen, t);
            break;
        case RIL_REQUEST_QUERY_AVAILABLE_NETWORKS:
            requestQueryAvailableNetworks(data, datalen, t);
            break;
        case RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE:
            requestSetPreferredNetworkType(data, datalen, t);
            break;
        case RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE:
            requestGetPreferredNetworkType(data, datalen, t);
            break;
        case RIL_REQUEST_REGISTRATION_STATE:
            requestRegistrationState(request, data, datalen, t);
	    break;
        case RIL_REQUEST_GPRS_REGISTRATION_STATE:
            requestGprsRegistrationState(request, data, datalen, t);
            break;
        /* case RIL_REQUEST_SET_LOCATION_UPDATES:
            requestSetLocationUpdates(data, datalen, t);
            break; */

        /* OEM */
        /* case RIL_REQUEST_OEM_HOOK_RAW:
            requestOEMHookRaw(data, datalen, t);
            break; */
        case RIL_REQUEST_OEM_HOOK_STRINGS:
            requestOEMHookStrings(data, datalen, t);
            break;

        /* Misc */
        case RIL_REQUEST_SIGNAL_STRENGTH:
            requestSignalStrength(data, datalen, t);
            break;
        case RIL_REQUEST_OPERATOR:
            requestOperator(data, datalen, t);
            break;
        case RIL_REQUEST_RADIO_POWER:
            requestRadioPower(data, datalen, t);
            break;
        case RIL_REQUEST_GET_IMSI:
            requestGetIMSI(data, datalen, t);
            break;
        case RIL_REQUEST_GET_IMEI:                  /* Deprecated */
            requestGetIMEI(data, datalen, t);
            break;
        case RIL_REQUEST_GET_IMEISV:                /* Deprecated */
            requestGetIMEISV(data, datalen, t);
            break;
        case RIL_REQUEST_DEVICE_IDENTITY:
            requestDeviceIdentity(data, datalen, t);
	    break;
        case RIL_REQUEST_BASEBAND_VERSION:
            requestBasebandVersion(data, datalen, t);
            break;
        /* case RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION:
            requestSetSuppSvcNotification(data, datalen, t);
            break; */

        /* SIM Application Toolkit */
	/*
        case RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE:
            requestStkSendTerminalResponse(data, datalen, t);
            break;
        case RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND:
            requestStkSendEnvelopeCommand(data, datalen, t);
            break;
        case RIL_REQUEST_STK_GET_PROFILE:
            requestStkGetProfile(data, datalen, t);
            break;
        case RIL_REQUEST_STK_SET_PROFILE:
            requestStkSetProfile(data, datalen, t);
            break;
        case RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM:
            requestStkHandleCallSetupRequestedFromSIM(data, datalen, t);
            break; */

        default:
            LOGW("FIXME: Unsupported request logged: %s",
                 requestToString(request));
            RIL_onRequestComplete(t, RIL_E_REQUEST_NOT_SUPPORTED, NULL, 0);
            break;
    }
}

/*** Callback methods from the RIL library to us ***/

/**
 * Call from RIL to us to make a RIL_REQUEST.
 *
 * Must be completed with a call to RIL_onRequestComplete().
 */
static void onRequest(int request, void *data, size_t datalen, RIL_Token t)
{
    RILRequest *r;
    RequestQueue *q = &s_requestQueue;

    if (s_requestQueuePrio.enabled && isPrioRequest(request))
        q = &s_requestQueuePrio;

    r = malloc(sizeof(RILRequest));  
    memset(r, 0, sizeof(RILRequest));

    /* Formulate a RILRequest and put it in the queue. */
    r->request = request;
    r->data = dupRequestData(request, data, datalen);
    r->datalen = datalen;
    r->token = t;

    pthread_mutex_lock(&q->queueMutex);

    /* Queue empty, just throw r on top. */
    if (q->requestList == NULL) {
        q->requestList = r;
    } else {
        RILRequest *l = q->requestList;
        while (l->next != NULL)
            l = l->next;

        l->next = r;
    }

    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->queueMutex);
}

/**
 * Synchronous call from the RIL to us to return current radio state.
 * RADIO_STATE_UNAVAILABLE should be the initial state.
 */
RIL_RadioState currentState()
{
    return sState;
}

/**
 * Call from RIL to us to find out whether a specific request code
 * is supported by this implementation.
 *
 * Return 1 for "supported" and 0 for "unsupported".
 *
 * Currently just stubbed with the default value of one. This is currently
 * not used by android, and therefore not implemented here. We return
 * RIL_E_REQUEST_NOT_SUPPORTED when we encounter unsupported requests.
 */
static int onSupports(int requestCode)
{
    (void) requestCode;
    LOGI("onSupports() called!");

    return 1;
}

/** 
 * onCancel() is currently stubbed, because android doesn't use it and
 * our implementation will depend on how a cancellation is handled in 
 * the upper layers.
 */
static void onCancel(RIL_Token t)
{
    (void) t;
    LOGI("onCancel() called!");
}

static const char *getVersion(void)
{
    return RIL_VERSION_STRING;
}

void setRadioState(RIL_RadioState newState)
{
    RIL_RadioState oldState;

    pthread_mutex_lock(&s_state_mutex);

    oldState = sState;

   if (sState != newState) {
        sState = newState;
    }

    pthread_mutex_unlock(&s_state_mutex);

    /* Do these outside of the mutex. */
    if (sState != oldState || sState == RADIO_STATE_SIM_LOCKED_OR_ABSENT) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
                                  NULL, 0);

        if (sState == RADIO_STATE_SIM_READY) {
            enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, onSIMReady, NULL, NULL);
        } else if (sState == RADIO_STATE_SIM_NOT_READY) {
            enqueueRILEvent(RIL_EVENT_QUEUE_NORMAL, onRadioPowerOn, NULL, NULL);
        }
    }
}

/** Returns 1 if on, 0 if off, and -1 on error. */
static int isRadioOn()
{
    ATResponse *atresponse = NULL;
    int err;
    char *line;
    int ret;

    err = at_send_command_singleline("AT+CFUN?", "+CFUN:", &atresponse);
    if (err < 0 || atresponse->success == 0) {
        /* Assume radio is off. */
        goto error;
    }

    line = atresponse->p_intermediates->line;

    err = at_tok_start(&line);
    if (err < 0)
        goto error;

    err = at_tok_nextint(&line, &ret);
    if (err < 0)
        goto error;

    switch (ret) {
        case 1:         /* Full functionality (switched on) */
        case 5:         /* GSM only */
        case 6:         /* WCDMA only */
            ret = 1;
            break;

        default:
            ret = 0;
    }

    at_response_free(atresponse);

    return ret;

error:
    at_response_free(atresponse);
    return -1;
}

static char initializeCommon(void)
{
    int err = 0;

    set_pending_hotswap(0);
    s_e2napState = -1;
    s_e2napCause = -1;

    if (at_handshake() < 0) {
        LOG_FATAL("Handshake failed!");
        goto error;
    }

    /* Configure/set
     *   command echo (E), result code suppression (Q), DCE response format (V)
     *
     *  E0 = DCE does not echo characters during command state and online
     *       command state
     *  Q0 = DCE transmits result codes
     *  V1 = Display verbose result codes
     */
    err = at_send_command("ATE0Q0V1", NULL);
    if (err < 0)
        goto error;

   /* Set default character set. */
    err = at_send_command("AT+CSCS=\"UTF-8\"", NULL);
    if (err < 0)
        goto error;

    /* Disable automatic answer. */
    err = at_send_command("ATS0=0", NULL);
    if (err < 0)
        goto error;

    /* Enable +CME ERROR: <err> result code and use numeric <err> values. */
    err = at_send_command("AT+CMEE=1", NULL);
    if (err < 0)
        goto error;

    err = at_send_command("AT*E2NAP=1", NULL);
    /* TODO: this command may return CME error */
    if (err < 0)
        goto error;

    /* Try to register for hotswap events. Don't care if it fails. */
    err = at_send_command("AT*EESIMSWAP=1", NULL);

    /* Enable Connected Line Identification Presentation. */
    err = at_send_command("AT+COLP=0", NULL);
    if (err < 0)
        goto error;

    /* Disable Service Reporting. */
    err = at_send_command("AT+CR=0", NULL);
    if (err < 0)
        goto error;

    /* Configure carrier detect signal - 1 = DCD follows the connection. */
    err = at_send_command("AT&C=1", NULL);
    if (err < 0)
        goto error;

    /* Configure DCE response to Data Termnal Ready signal - 0 = ignore. */
    err = at_send_command("AT&D=0", NULL);
    if (err < 0)
        goto error;

    /* Configure Cellular Result Codes - 0 = Disables extended format. */
    err = at_send_command("AT+CRC=0", NULL);
    if (err < 0)
        goto error;

    /* Configure Bearer Service Type and HSCSD Non-Transparent Call
     *  +CBST
     *     7 = 9600 bps V.32
     *     0 = Asynchronous connection
     *     1 = Non-transparent connection element
     *  +CHSN
     *     1 = Wanted air interface user rate is 9,6 kbits/s
     *     1 = Wanted number of receive timeslots is 1
     *     0 = Indicates that the user is not going to change <wAiur> and /or 
     *         <wRx> during the next call
     *     4 = Indicates that the accepted channel coding for the next
     *         established non-transparent HSCSD call is 9,6 kbit/s only
     */
    err = at_send_command("AT+CBST=7,0,1;+CHSN=1,1,0,4", NULL);
    if (err < 0)
        goto error;

    /* Configure Call progress Monitoring
     *    3 = BUSY result code given if called line is busy. 
     *        No NO DIALTONE result code is given.
     *        Reports line speed together with CONNECT result code.
     */
    err = at_send_command("ATX3", NULL);
    if (err < 0)
        goto error;

    return 0;
error:
    return 1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0.
 */
static char initializeChannel()
{
    int err;

    LOGI("initializeChannel()");

    setRadioState(RADIO_STATE_OFF);

    /* Configure Packet Domain Network Registration Status events
     *    2 = Enable network registration and location information
     *        unsolicited result code
     */
    err = at_send_command("AT+CGREG=2", NULL);
    if (err < 0)
        goto error;

    /* Set phone functionality.
     *    4 = Disable the phone's transmit and receive RF circuits.
     */
    err = at_send_command("AT+CFUN=4", NULL);
    if (err < 0)
        goto error;

    /* Assume radio is off on error. */
    if (isRadioOn() > 0) {
        setRadioState(RADIO_STATE_SIM_NOT_READY);
    }

    return 0;

error:
    return 1;
}

/**
 * Initialize everything that can be configured while we're still in
 * AT+CFUN=0.
 */
static char initializePrioChannel()
{
    int err;

    LOGI("initializePrioChannel()");

    /* Subscribe to ST-Ericsson Pin code event.
     *   The command requests the MS to report when the PIN code has been
     *   inserted and accepted.
     *      1 = Request for report on inserted PIN code is activated (on) 
     */
    err = at_send_command("AT*EPEE=1", NULL);
    if (err < 0)
        return 1;

    return 0;
}

/**
 * Called by atchannel when an unsolicited line appears.
 * This is called on atchannel's reader thread. AT commands may
 * not be issued here.
 */
static void onUnsolicited(const char *s, const char *sms_pdu)
{
    /* Ignore unsolicited responses until we're initialized.
       This is OK because the RIL library will poll for initial state. */
    if (sState == RADIO_STATE_UNAVAILABLE) {
        return;
    }

    if (strStartsWith(s, "*ETZV:")) {
        /* If we're in screen state, we have disabled CREG, but the ETZV
           will catch those few cases. So we send network state changed as
           well on NITZ. */
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
                                  NULL, 0);

        onNetworkTimeReceived(s);
    } else if (strStartsWith(s, "*EPEV")) {
        /* Pin event, poll SIM State! */
        enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, pollSIMState, NULL, NULL);
    } else if (strStartsWith(s, "*ESIMSR")) {
        onSimStateChanged(s);
    }
    else if(strStartsWith(s, "*E2NAP:")) {
        onConnectionStateChanged(s);
    } else if (strStartsWith(s, "*EESIMSWAP:")) {
        onSimHotswap(s);
    }
    else if (strStartsWith(s, "+CRING:")
               || strStartsWith(s, "RING")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_CALL_RING, NULL, 0);
    } else if (strStartsWith(s, "NO CARRIER")
               || strStartsWith(s, "+CCWA")
               || strStartsWith(s, "BUSY")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
                                  NULL, 0);
    } else if (strStartsWith(s, "+CREG:")
	    || strStartsWith(s, "+CGREG:")) {
/*TODO: If only reporting back network change Android can sometimes hang!!/*/
        RIL_onUnsolicitedResponse(RIL_UNSOL_RESPONSE_NETWORK_STATE_CHANGED,
                                  NULL, 0);
    } else if (strStartsWith(s, "+CMT:")) {
        onNewSms(sms_pdu);
    } else if (strStartsWith(s, "+CBM:")) {
        onNewBroadcastSms(sms_pdu);
    } else if (strStartsWith(s, "+CMTI:")) {
        onNewSmsOnSIM(s);
    } else if (strStartsWith(s, "+CDS:")) {
        onNewStatusReport(sms_pdu);
    /* } else if (strStartsWith(s, "+CGEV:")) { */
    /*     /\* Really, we can ignore NW CLASS and ME CLASS events here, */
    /*        but right now we don't since extranous */
    /*        RIL_UNSOL_PDP_CONTEXT_LIST_CHANGED calls are tolerated. *\/ */
    /*     enqueueRILEvent(RIL_EVENT_QUEUE_NORMAL, onPDPContextListChanged, */
    /*                     NULL, NULL); */
    } else if (strStartsWith(s, "+CIEV: 2")) {
	    onSignalStrengthChanged(s);
    } else if (strStartsWith(s, "+CSSI:")) {
        onSuppServiceNotification(s, 0);
    } else if (strStartsWith(s, "+CSSU:")) {
        onSuppServiceNotification(s, 1);
    } else if (strStartsWith(s, "+CUSD:")) {
        onUSSDReceived(s);
    } else if (strStartsWith(s, "*STKEND")) {
        RIL_onUnsolicitedResponse(RIL_UNSOL_STK_SESSION_END, NULL, 0);
    }
}

static void signalCloseQueues(void)
{
    unsigned int i;
    for (i = 0; i < (sizeof(s_requestQueues) / sizeof(RequestQueue *)); i++) {
        RequestQueue *q = s_requestQueues[i];
        pthread_mutex_lock(&q->queueMutex);
        q->closed = 1;
        pthread_cond_signal(&q->cond);
        pthread_mutex_unlock(&q->queueMutex);
    }
}

/* Called on command or reader thread. */
static void onATReaderClosed()
{
    LOGI("AT channel closed\n");

    if (!get_pending_hotswap())
        setRadioState(RADIO_STATE_UNAVAILABLE);
    signalCloseQueues();
}

/* Called on command thread. */
static void onATTimeout()
{
    LOGI("AT channel timeout; restarting..\n");
    /* Last resort, throw escape on the line, close the channel
       and hope for the best. */
    at_send_escape();

    setRadioState(RADIO_STATE_UNAVAILABLE);
    signalCloseQueues();

    /* TODO We may cause a radio reset here. */
}

static void onConnectionStateChanged(const char *s)
{
    int m_state = -1, m_cause = -1, err;

    err = at_tok_start((char **) &s);
    if (err < 0)
        return;

    err = at_tok_nextint((char **) &s, &m_state);
    if (err < 0 || m_state < E2NAP_ST_DISCONNECTED || m_state > E2NAP_ST_CONNECTED) {
        m_state = -1;
        return;
    }

    err = at_tok_nextint((char **) &s, &m_cause);
    /* The <cause> will only be indicated when <state> is disconnected */
    if (err < 0 || m_cause < E2NAP_C_SUCCESS || m_cause > E2NAP_C_MAXIMUM)
        m_cause = -1;

    pthread_mutex_lock(&s_e2nap_mutex);
    s_e2napState = m_state;
    s_e2napCause = m_cause;
    pthread_mutex_unlock(&s_e2nap_mutex);

    LOGD("onConnectionStateChanged: %s",e2napStateToString(m_state));
    if (m_state != E2NAP_ST_CONNECTING)
        enqueueRILEvent(RIL_EVENT_QUEUE_PRIO, onPDPContextListChanged, NULL, NULL);

    mbm_check_error_cause();
}

static void usage(char *s)
{
    fprintf(stderr, "usage: %s [-z] [-p <tcp port>] [-d /dev/tty_device] [-x /dev/tty_device] [-i <network interface>]\n", s);
    exit(-1);
}

struct queueArgs {
    int port;
    char * loophost;
    const char *device_path;
    char isPrio;
    char hasPrio;
};

static int safe_read(int fd, char *buf, int count)
{
	int n;
	int i = 0;

	while (i < count) {
		n = read(fd, buf + i, count - i);
		if (n > 0)
			i += n;
		else if (!(n < 0 && errno == EINTR))
			return -1;
	}

	return count;
}

#define TIMEOUT_SEARCH_FOR_TTY 5 /* Poll every Xs for the port*/
#define TIMEOUT_EMRDY 10 /* Module should respond at least within 10s */
#define MAX_BUF 1024
static void *queueRunner(void *param)
{
	int fd = -1;
	int ret = 0;
	int n;
	fd_set input;
	struct timeval timeout;
	int max_fd = -1;
	char start[MAX_BUF];
	struct queueArgs *queueArgs = (struct queueArgs *) param;
	struct RequestQueue *q = NULL;
	
	LOGI("queueRunner: starting!");
	
	for (;;) {
		fd = -1;
		max_fd = -1;
		while (fd < 0) {
			if (queueArgs->port > 0) {
				if (queueArgs->loophost) {
					fd = socket_network_client(queueArgs->loophost, queueArgs->port, SOCK_STREAM);
				} else {
					fd = socket_loopback_client(queueArgs->port, SOCK_STREAM);
				}
			} else if (queueArgs->device_path != NULL) {
				/* Program is not controlling terminal -> O_NOCTTY */
				/* Dont care about DCD -> O_NDELAY */
				fd = open(queueArgs->device_path, O_RDWR | O_NOCTTY); /* | O_NDELAY); */
				if (fd >= 0 && !memcmp(queueArgs->device_path, "/dev/ttyA", 9)) {
					struct termios ios;
					/* Clear the struct and then call cfmakeraw*/
					tcflush(fd, TCIOFLUSH);
					tcgetattr(fd, &ios);
					memset(&ios, 0, sizeof(struct termios));
					cfmakeraw(&ios);
					/* OK now we are in a known state, set what we want*/
					ios.c_cflag |= CRTSCTS;
					/* ios.c_cc[VMIN]  = 0; */
					/* ios.c_cc[VTIME] = 1; */
					ios.c_cflag |= CS8;
					tcsetattr(fd, TCSANOW, &ios);
					tcflush(fd, TCIOFLUSH);
					tcsetattr(fd, TCSANOW, &ios);
					tcflush(fd, TCIOFLUSH);
					tcflush(fd, TCIOFLUSH);
					cfsetospeed(&ios, B115200);
					cfsetispeed(&ios, B115200);
					tcsetattr(fd, TCSANOW, &ios);
					
				}
			}
			
			if (fd < 0) {
				LOGE("queueRunner: Failed to open AT channel %s (%s), retrying in %d.", 
					queueArgs->device_path, strerror(errno), TIMEOUT_SEARCH_FOR_TTY);
				sleep(TIMEOUT_SEARCH_FOR_TTY);
				/* Never returns. */
			}
		}
		
		/* Reset the blocking mode*/
		fcntl(fd, F_SETFL, 0);
		FD_ZERO(&input);
		FD_SET(fd, &input);
		if (fd >= max_fd)
			max_fd = fd + 1;

		timeout.tv_sec = TIMEOUT_EMRDY;
		timeout.tv_usec = 0;
		
		LOGI("queueRunner: waiting for emrdy...");
		n = select(max_fd, &input, NULL, NULL, &timeout);
		
		if (n < 0) {
			LOGE("queueRunner: Select error");
			return NULL;
		} else if (n == 0) {
			LOGE("queueRunner: timeout, go ahead anyway(might work)...");
		} else {
			memset(start, 0, MAX_BUF);
			safe_read(fd, start, MAX_BUF-1);
			
			if (start == NULL) {
				LOGI("queueRunner: Eiii empty string");
				tcflush(fd, TCIOFLUSH);
				FD_CLR(fd, &input);
				close(fd);
				continue;
			}
						
			if (strstr(start, "EMRDY") == NULL) {
				LOGI("queueRunner: Eiii this was not EMRDY: %s", start);
				tcflush(fd, TCIOFLUSH);
				FD_CLR(fd, &input);
				close(fd);
				continue;
			}
			
			LOGI("queueRunner: Got EMRDY");
		}
		
		ret = at_open(fd, onUnsolicited);
		
		if (ret < 0) {
			LOGE("queueRunner: AT error %d on at_open\n", ret);
			at_close();
			continue;
		}
		
		at_set_on_reader_closed(onATReaderClosed);
		at_set_on_timeout(onATTimeout);
		
		q = &s_requestQueue;
		
		if(initializeCommon()) {
			LOGE("queueRunner: Failed to initialize channel!");
			at_close();
			continue;
		}
		
		if (queueArgs->isPrio == 0) {
			q->closed = 0;
			if (initializeChannel()) {
				LOGE("queueRunner: Failed to initialize channel!");
				at_close();
				continue;
			}
			at_make_default_channel();
		} else {
			q = &s_requestQueuePrio;
			q->closed = 0;
			at_set_timeout_msec(1000 * 30); 
		}
		
		if (queueArgs->hasPrio == 0 || queueArgs->isPrio)
			if (initializePrioChannel()) {
				LOGE("queueRunner: Failed to initialize channel!");
				at_close();
				continue;
			}
		
		LOGE("queueRunner: Looping the requestQueue!");
		for (;;) {
			RILRequest *r;
			RILEvent *e;
			struct timeval tv;
			struct timespec ts;
			
			memset(&ts, 0, sizeof(ts));
			
			pthread_mutex_lock(&q->queueMutex);
			
			if (q->closed != 0) {
				LOGW("queueRunner: AT Channel error, attempting to recover..");
				pthread_mutex_unlock(&q->queueMutex);
				break;
			}
			
			while (q->closed == 0 && q->requestList == NULL && 
				q->eventList == NULL) {
				pthread_cond_wait(&q->cond,
						&q->queueMutex);
			}
			
			/* eventList is prioritized, smallest abstime first. */
			if (q->closed == 0 && q->requestList == NULL && q->eventList) {
				int err = 0;
				err = pthread_cond_timedwait(&q->cond, &q->queueMutex, &q->eventList->abstime);
				if (err && err != ETIMEDOUT)
					LOGE("queueRunner: timedwait returned unexpected error: %s",
						strerror(err));
			}
			
			if (q->closed != 0) {
				pthread_mutex_unlock(&q->queueMutex);
				continue; /* Catch the closed bit at the top of the loop. */
			}
			
			e = NULL;
			r = NULL;
			
			gettimeofday(&tv, NULL);
			
			/* Saves a macro, uses some stack and clock cycles.
			   TODO Might want to change this. */
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = tv.tv_usec * 1000;
			
			if (q->eventList != NULL &&
				timespec_cmp(q->eventList->abstime, ts, < )) {
				e = q->eventList;
				q->eventList = e->next;
			}
			
			if (q->requestList != NULL) {
				r = q->requestList;
				q->requestList = r->next;
			}
			
			pthread_mutex_unlock(&q->queueMutex);
			
			if (e) {
				e->eventCallback(e->param);
				free(e);
			}
			
			if (r) {
				processRequest(r->request, r->data, r->datalen, r->token);
				freeRequestData(r->request, r->data, r->datalen);
				free(r);
			}
		}
		
		at_close();
		LOGE("queueRunner: Re-opening after close");
	}
	return NULL;
}

pthread_t s_tid_queueRunner;
pthread_t s_tid_queueRunnerPrio;

void dummyFunction(void *args)
{
    LOGE("dummyFunction: %p", args);
}

const RIL_RadioFunctions *RIL_Init(const struct RIL_Env *env, int argc,
                                   char **argv)
{
    int opt;
    int port = -1;
    char *loophost = NULL;
    const char *device_path = NULL;
    const char *priodevice_path = NULL;
    struct queueArgs *queueArgs;
    struct queueArgs *prioQueueArgs;
    pthread_attr_t attr;

    s_rilenv = env;

    LOGI("RIL_Init: entering...");

    while (-1 != (opt = getopt(argc, argv, "z:i:p:d:s:x:"))) {
        switch (opt) {
            case 'z':
                loophost = optarg;
                LOGI("RIL_Init: Using loopback host %s..", loophost);
                break;

            case 'i':
                ril_iface = optarg;
                LOGI("RIL_Init: Using network interface %s as primary data channel.",
                     ril_iface);
                break;

            case 'p':
                port = atoi(optarg);
                if (port == 0) {
                    usage(argv[0]);
                    return NULL;
                }
                LOGI("RIL_Init: Opening loopback port %d\n", port);
                break;

            case 'd':
                device_path = optarg;
                LOGI("RIL_Init: Opening tty device %s\n", device_path);
                break;

            case 'x':
                priodevice_path = optarg;
                LOGI("RIL_Init: Opening priority tty device %s\n", priodevice_path);
                break;
            default:
                usage(argv[0]);
                return NULL;
        }
    }

    if (ril_iface == NULL) {
        LOGI("RIL_Init: Network interface was not supplied, falling back on usb0!");
        ril_iface = strdup("usb0\0");
    }

    if (port < 0 && device_path == NULL) {
        usage(argv[0]);
        return NULL;
    }

    queueArgs = malloc(sizeof(struct queueArgs));
    memset(queueArgs, 0, sizeof(struct queueArgs));

    queueArgs->device_path = device_path;
    queueArgs->port = port;
    queueArgs->loophost = loophost;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (priodevice_path != NULL) {
        prioQueueArgs = malloc(sizeof(struct queueArgs));
        memset(prioQueueArgs, 0, sizeof(struct queueArgs));
        prioQueueArgs->device_path = priodevice_path;
        prioQueueArgs->isPrio = 1;
        prioQueueArgs->hasPrio = 1;
        queueArgs->hasPrio = 1;

        s_requestQueuePrio.enabled = 1;

        pthread_create(&s_tid_queueRunnerPrio, &attr, queueRunner, prioQueueArgs);
    }

    pthread_create(&s_tid_queueRunner, &attr, queueRunner, queueArgs);
    
    return &s_callbacks;
}
