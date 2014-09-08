/* imdiag.c
 * This is a testbench tool. It started out with a broader scope,
 * but we dropped this idea. To learn about rsyslog runtime statistics
 * have a look at impstats.
 *
 * File begun on 2008-07-25 by RGerhards
 *
 * Copyright 2008-2014 Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>
#include <ctype.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include "rsyslog.h"
#include "dirty.h"
#include "cfsysline.h"
#include "module-template.h"
#include "unicode-helper.h"
#include "net.h"
#include "netstrm.h"
#include "errmsg.h"
#include "tcpsrv.h"
#include "srUtils.h"
#include "msg.h"
#include "datetime.h"
#include "ratelimit.h"
#include "net.h" /* for permittedPeers, may be removed when this is removed */

MODULE_TYPE_INPUT
MODULE_TYPE_NOKEEP

/* static data */
DEF_IMOD_STATIC_DATA
DEFobjCurrIf(tcpsrv)
DEFobjCurrIf(tcps_sess)
DEFobjCurrIf(net)
DEFobjCurrIf(netstrm)
DEFobjCurrIf(errmsg)
DEFobjCurrIf(datetime)
DEFobjCurrIf(prop)

/* Module static data */
static tcpsrv_t *pOurTcpsrv = NULL;  /* our TCP server(listener) TODO: change for multiple instances */
static permittedPeers_t *pPermPeersRoot = NULL;
static prop_t *pInputName = NULL;	/* there is only one global inputName for all messages generated by this input */
static prop_t *pRcvDummy = NULL;
static prop_t *pRcvIPDummy = NULL;


/* config settings */
struct modConfData_s {
	EMPTY_STRUCT;
};

static int iTCPSessMax = 20; /* max number of sessions */
static int iStrmDrvrMode = 0; /* mode for stream driver, driver-dependent (0 mostly means plain tcp) */
static uchar *pszStrmDrvrAuthMode = NULL; /* authentication mode to use */
static uchar *pszInputName = NULL; /* value for inputname property, NULL is OK and handled by core engine */


/* callbacks */
/* this shall go into a specific ACL module! */
static int
isPermittedHost(struct sockaddr __attribute__((unused)) *addr, char __attribute__((unused)) *fromHostFQDN,
		void __attribute__((unused)) *pUsrSrv, void __attribute__((unused)) *pUsrSess)
{
	return 1;	/* TODO: implement ACLs ... or via some other way? */
}


static rsRetVal
doOpenLstnSocks(tcpsrv_t *pSrv)
{
	ISOBJ_TYPE_assert(pSrv, tcpsrv);
	return tcpsrv.create_tcp_socket(pSrv);
}


static rsRetVal
doRcvData(tcps_sess_t *pSess, char *buf, size_t lenBuf, ssize_t *piLenRcvd)
{
	DEFiRet;
	assert(pSess != NULL);
	assert(piLenRcvd != NULL);

	*piLenRcvd = lenBuf;
	CHKiRet(netstrm.Rcv(pSess->pStrm, (uchar*) buf, piLenRcvd));
finalize_it:
	RETiRet;
}

static rsRetVal
onRegularClose(tcps_sess_t *pSess)
{
	DEFiRet;
	assert(pSess != NULL);

	/* process any incomplete frames left over */
	tcps_sess.PrepareClose(pSess);
	/* Session closed */
	tcps_sess.Close(pSess);
	RETiRet;
}


static rsRetVal
onErrClose(tcps_sess_t *pSess)
{
	DEFiRet;
	assert(pSess != NULL);

	tcps_sess.Close(pSess);
	RETiRet;
}

/* ------------------------------ end callbacks ------------------------------ */


/* get the first word delimited by space from a given string. The pointer is
 * advanced to after the word. Any leading spaces are discarded. If the
 * output buffer is too small, parsing ends on buffer full condition.
 * An empty buffer is returned if there is no more data inside the string.
 * rgerhards, 2009-05-27
 */
#define TO_LOWERCASE	1
#define NO_MODIFY	0
static void
getFirstWord(uchar **ppszSrc, uchar *pszBuf, size_t lenBuf, int options)
{
	uchar c;
	uchar *pszSrc = *ppszSrc;

	while(*pszSrc && *pszSrc == ' ')
		++pszSrc; /* skip to first non-space */

	while(*pszSrc && *pszSrc != ' ' && lenBuf > 1) {
		c = *pszSrc++;
		if(options & TO_LOWERCASE)
			c = tolower(c);
		*pszBuf++ = c;
		lenBuf--;
	}

	*pszBuf = '\0';
	*ppszSrc = pszSrc;
}


/* send a response back to the originator
 * rgerhards, 2009-05-27
 */
static rsRetVal __attribute__((format(printf, 2, 3)))
sendResponse(tcps_sess_t *pSess, char *fmt, ...)
{
	va_list ap;
	ssize_t len;
	uchar buf[1024];
	DEFiRet;

	va_start(ap, fmt);
	len = vsnprintf((char*)buf, sizeof(buf), fmt, ap);
	va_end(ap);
	CHKiRet(netstrm.Send(pSess->pStrm, buf, &len));

finalize_it:
	RETiRet;
}


/* actually submit a message to the rsyslog core
 */
static rsRetVal
doInjectMsg(int iNum, ratelimit_t *ratelimiter)
{
	uchar szMsg[1024];
	msg_t *pMsg;
	struct syslogTime stTime;
	time_t ttGenTime;
	DEFiRet;

	snprintf((char*)szMsg, sizeof(szMsg)/sizeof(uchar),
		 "<167>Mar  1 01:00:00 172.20.245.8 tag msgnum:%8.8d:", iNum);

	datetime.getCurrTime(&stTime, &ttGenTime);
	/* we now create our own message object and submit it to the queue */
	CHKiRet(msgConstructWithTime(&pMsg, &stTime, ttGenTime));
	MsgSetRawMsg(pMsg, (char*) szMsg, ustrlen(szMsg));
	MsgSetInputName(pMsg, pInputName);
	MsgSetFlowControlType(pMsg, eFLOWCTL_NO_DELAY);
	pMsg->msgFlags  = NEEDS_PARSING | PARSE_HOSTNAME;
	MsgSetRcvFrom(pMsg, pRcvDummy);
	CHKiRet(MsgSetRcvFromIP(pMsg, pRcvIPDummy));
	CHKiRet(ratelimitAddMsg(ratelimiter, NULL, pMsg));

finalize_it:
	RETiRet;
}


/* This function injects messages. Command format:
 * injectmsg <fromnbr> <number-of-messages>
 * rgerhards, 2009-05-27
 */
static rsRetVal
injectMsg(uchar *pszCmd, tcps_sess_t *pSess)
{
	uchar wordBuf[1024];
	int iFrom;
	int nMsgs;
	int i;
	ratelimit_t *ratelimit = NULL;
	DEFiRet;

	/* we do not check errors here! */
	getFirstWord(&pszCmd, wordBuf, sizeof(wordBuf)/sizeof(uchar), TO_LOWERCASE);
	iFrom = atoi((char*)wordBuf);
	getFirstWord(&pszCmd, wordBuf, sizeof(wordBuf)/sizeof(uchar), TO_LOWERCASE);
	nMsgs = atoi((char*)wordBuf);
	CHKiRet(ratelimitNew(&ratelimit, "imdiag", "injectmsg"));

	for(i = 0 ; i < nMsgs ; ++i) {
		doInjectMsg(i + iFrom, ratelimit);
	}

	CHKiRet(sendResponse(pSess, "%d messages injected\n", nMsgs));
	DBGPRINTF("imdiag: %d messages injected\n", nMsgs);

finalize_it:
	if(ratelimit != NULL)
		ratelimitDestruct(ratelimit);
	RETiRet;
}


/* This function waits until the main queue is drained (size = 0)
 * To make sure it really is drained, we check three times. Otherwise we
 * may just see races.
 */
static rsRetVal
waitMainQEmpty(tcps_sess_t *pSess)
{
	int iMsgQueueSize;
	int iPrint = 0;
	DEFiRet;

	CHKiRet(diagGetMainMsgQSize(&iMsgQueueSize));
	while(1) {
		if(iMsgQueueSize == 0) {
			/* verify that queue is still empty (else it could just be a race!) */
			srSleep(0,250000);/* wait a little bit */
			CHKiRet(diagGetMainMsgQSize(&iMsgQueueSize));
			if(iMsgQueueSize == 0) {
				srSleep(0,500000);/* wait a little bit */
				CHKiRet(diagGetMainMsgQSize(&iMsgQueueSize));
			}
		}
		if(iMsgQueueSize == 0)
			break;
		if(iPrint++ % 500 == 0) 
			dbgprintf("imdiag sleeping, wait mainq drain, curr size %d\n", iMsgQueueSize);
		srSleep(0,200000);/* wait a little bit */
		CHKiRet(diagGetMainMsgQSize(&iMsgQueueSize));
	}

	CHKiRet(sendResponse(pSess, "mainqueue empty\n"));
	DBGPRINTF("imdiag: mainqueue empty\n");

finalize_it:
	RETiRet;
}

/* Function to handle received messages. This is our core function!
 * rgerhards, 2009-05-24
 */
static rsRetVal
OnMsgReceived(tcps_sess_t *pSess, uchar *pRcv, int iLenMsg)
{
	int iMsgQueueSize;
	uchar *pszMsg;
	uchar *pToFree = NULL;
	uchar cmdBuf[1024];
	DEFiRet;

	assert(pSess != NULL);
	assert(pRcv != NULL);

	/* NOTE: pRcv is NOT a C-String but rather an array of characters
	 * WITHOUT a termination \0 char. So we need to convert it to one
	 * before proceeding.
	 */
	CHKmalloc(pszMsg = MALLOC(sizeof(uchar) * (iLenMsg + 1)));
	pToFree = pszMsg;
	memcpy(pszMsg, pRcv, iLenMsg);
	pszMsg[iLenMsg] = '\0';

	getFirstWord(&pszMsg, cmdBuf, sizeof(cmdBuf)/sizeof(uchar), TO_LOWERCASE);

	dbgprintf("imdiag received command '%s'\n", cmdBuf);
	if(!ustrcmp(cmdBuf, UCHAR_CONSTANT("getmainmsgqueuesize"))) {
		CHKiRet(diagGetMainMsgQSize(&iMsgQueueSize));
		CHKiRet(sendResponse(pSess, "%d\n", iMsgQueueSize));
		DBGPRINTF("imdiag: %d messages in main queue\n", iMsgQueueSize);
	} else if(!ustrcmp(cmdBuf, UCHAR_CONSTANT("waitmainqueueempty"))) {
		CHKiRet(waitMainQEmpty(pSess));
	} else if(!ustrcmp(cmdBuf, UCHAR_CONSTANT("injectmsg"))) {
		CHKiRet(injectMsg(pszMsg, pSess));
	} else {
		dbgprintf("imdiag unkown command '%s'\n", cmdBuf);
		CHKiRet(sendResponse(pSess, "unkown command '%s'\n", cmdBuf));
	}

finalize_it:
	if(pToFree != NULL)
		free(pToFree);
	RETiRet;
}


/* set permitted peer -- rgerhards, 2008-05-19
 */
static rsRetVal
setPermittedPeer(void __attribute__((unused)) *pVal, uchar *pszID)
{
	DEFiRet;
	CHKiRet(net.AddPermittedPeer(&pPermPeersRoot, pszID));
	free(pszID); /* no longer needed, but we need to free as of interface def */
finalize_it:
	RETiRet;
}


static rsRetVal addTCPListener(void __attribute__((unused)) *pVal, uchar *pNewVal)
{
	DEFiRet;

	if(pOurTcpsrv == NULL) {
		CHKiRet(tcpsrv.Construct(&pOurTcpsrv));
		CHKiRet(tcpsrv.SetSessMax(pOurTcpsrv, iTCPSessMax));
		CHKiRet(tcpsrv.SetCBIsPermittedHost(pOurTcpsrv, isPermittedHost));
		CHKiRet(tcpsrv.SetCBRcvData(pOurTcpsrv, doRcvData));
		CHKiRet(tcpsrv.SetCBOpenLstnSocks(pOurTcpsrv, doOpenLstnSocks));
		CHKiRet(tcpsrv.SetCBOnRegularClose(pOurTcpsrv, onRegularClose));
		CHKiRet(tcpsrv.SetCBOnErrClose(pOurTcpsrv, onErrClose));
		CHKiRet(tcpsrv.SetDrvrMode(pOurTcpsrv, iStrmDrvrMode));
		CHKiRet(tcpsrv.SetOnMsgReceive(pOurTcpsrv, OnMsgReceived));
		/* now set optional params, but only if they were actually configured */
		if(pszStrmDrvrAuthMode != NULL) {
			CHKiRet(tcpsrv.SetDrvrAuthMode(pOurTcpsrv, pszStrmDrvrAuthMode));
		}
		if(pPermPeersRoot != NULL) {
			CHKiRet(tcpsrv.SetDrvrPermPeers(pOurTcpsrv, pPermPeersRoot));
		}
	}

	/* initialized, now add socket */
	CHKiRet(tcpsrv.SetInputName(pOurTcpsrv, pszInputName == NULL ?
						UCHAR_CONSTANT("imdiag") : pszInputName));
	CHKiRet(tcpsrv.SetOrigin(pOurTcpsrv, (uchar*)"imdiag"));
	/* we support octect-cuunted frame (constant 1 below) */
	tcpsrv.configureTCPListen(pOurTcpsrv, pNewVal, 1);

finalize_it:
	if(iRet != RS_RET_OK) {
		errmsg.LogError(0, NO_ERRCODE, "error %d trying to add listener", iRet);
		if(pOurTcpsrv != NULL)
			tcpsrv.Destruct(&pOurTcpsrv);
	}
	free(pNewVal);
	RETiRet;
}


#if 0 /* can be used to integrate into new config system */
BEGINbeginCnfLoad
CODESTARTbeginCnfLoad
ENDbeginCnfLoad


BEGINendCnfLoad
CODESTARTendCnfLoad
ENDendCnfLoad


BEGINcheckCnf
CODESTARTcheckCnf
ENDcheckCnf


BEGINactivateCnf
CODESTARTactivateCnf
ENDactivateCnf


BEGINfreeCnf
CODESTARTfreeCnf
ENDfreeCnf
#endif

/* This function is called to gather input.
 */
BEGINrunInput
CODESTARTrunInput
	CHKiRet(tcpsrv.ConstructFinalize(pOurTcpsrv));
	iRet = tcpsrv.Run(pOurTcpsrv);
finalize_it:
ENDrunInput


/* initialize and return if will run or not */
BEGINwillRun
CODESTARTwillRun
	/* first apply some config settings */
	if(pOurTcpsrv == NULL)
		ABORT_FINALIZE(RS_RET_NO_RUN);
	/* we need to create the inputName property (only once during our lifetime) */
	CHKiRet(prop.Construct(&pInputName));
	CHKiRet(prop.SetString(pInputName, UCHAR_CONSTANT("imdiag"), sizeof("imdiag") - 1));
	CHKiRet(prop.ConstructFinalize(pInputName));

	CHKiRet(prop.Construct(&pRcvDummy));
	CHKiRet(prop.SetString(pRcvDummy, UCHAR_CONSTANT("127.0.0.1"), sizeof("127.0.0.1") - 1));
	CHKiRet(prop.ConstructFinalize(pRcvDummy));

	CHKiRet(prop.Construct(&pRcvIPDummy));
	CHKiRet(prop.SetString(pRcvIPDummy, UCHAR_CONSTANT("127.0.0.1"), sizeof("127.0.0.1") - 1));
	CHKiRet(prop.ConstructFinalize(pRcvIPDummy));

finalize_it:
ENDwillRun


BEGINafterRun
CODESTARTafterRun
	if(pInputName != NULL)
		prop.Destruct(&pInputName);
	if(pRcvDummy != NULL)
		prop.Destruct(&pRcvDummy);
	if(pRcvIPDummy != NULL)
		prop.Destruct(&pRcvIPDummy);
ENDafterRun


BEGINmodExit
CODESTARTmodExit
	if(pOurTcpsrv != NULL)
		iRet = tcpsrv.Destruct(&pOurTcpsrv);

	if(pPermPeersRoot != NULL) {
		net.DestructPermittedPeers(&pPermPeersRoot);
	}

	/* free some globals to keep valgrind happy */
	free(pszInputName);

	/* release objects we used */
	objRelease(net, LM_NET_FILENAME);
	objRelease(netstrm, LM_NETSTRMS_FILENAME);
	objRelease(tcps_sess, LM_TCPSRV_FILENAME);
	objRelease(tcpsrv, LM_TCPSRV_FILENAME);
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(datetime, CORE_COMPONENT);
	objRelease(prop, CORE_COMPONENT);
ENDmodExit


static rsRetVal
resetConfigVariables(uchar __attribute__((unused)) *pp, void __attribute__((unused)) *pVal)
{
	iTCPSessMax = 200;
	iStrmDrvrMode = 0;
	free(pszInputName);
	pszInputName = NULL;
	if(pszStrmDrvrAuthMode != NULL) {
		free(pszStrmDrvrAuthMode);
		pszStrmDrvrAuthMode = NULL;
	}
	return RS_RET_OK;
}


BEGINisCompatibleWithFeature
CODESTARTisCompatibleWithFeature
	if(eFeat == sFEATURENonCancelInputTermination)
		iRet = RS_RET_OK;
ENDisCompatibleWithFeature


BEGINqueryEtryPt
CODESTARTqueryEtryPt
CODEqueryEtryPt_STD_IMOD_QUERIES
CODEqueryEtryPt_IsCompatibleWithFeature_IF_OMOD_QUERIES
ENDqueryEtryPt


BEGINmodInit()
CODESTARTmodInit
	*ipIFVersProvided = CURR_MOD_IF_VERSION; /* we only support the current interface specification */
CODEmodInit_QueryRegCFSLineHdlr
	pOurTcpsrv = NULL;
	/* request objects we use */
	CHKiRet(objUse(net, LM_NET_FILENAME));
	CHKiRet(objUse(netstrm, LM_NETSTRMS_FILENAME));
	CHKiRet(objUse(tcps_sess, LM_TCPSRV_FILENAME));
	CHKiRet(objUse(tcpsrv, LM_TCPSRV_FILENAME));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(prop, CORE_COMPONENT));

	/* register config file handlers */
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagserverrun"), 0, eCmdHdlrGetWord,
				   addTCPListener, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagmaxsessions"), 0, eCmdHdlrInt,
				   NULL, &iTCPSessMax, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagserverstreamdrivermode"), 0,
				   eCmdHdlrInt, NULL, &iStrmDrvrMode, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagserverstreamdriverauthmode"), 0,
				   eCmdHdlrGetWord, NULL, &pszStrmDrvrAuthMode, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagserverstreamdriverpermittedpeer"), 0,
				   eCmdHdlrGetWord, setPermittedPeer, NULL, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("imdiagserverinputname"), 0,
				   eCmdHdlrGetWord, NULL, &pszInputName, STD_LOADABLE_MODULE_ID));
	CHKiRet(omsdRegCFSLineHdlr(UCHAR_CONSTANT("resetconfigvariables"), 1, eCmdHdlrCustomHandler,
		resetConfigVariables, NULL, STD_LOADABLE_MODULE_ID));
ENDmodInit


/* vim:set ai:
 */
