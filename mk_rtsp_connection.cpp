
#include <sstream>
#include "mk_rtsp_connection.h"
#include "mk_rtsp_packet.h"
#include "mk_rtsp_service.h"
#include "mk_rtsp_message_options.h"

std::string mk_rtsp_connection::m_RtspCode[] = RTSP_CODE_STRING;
std::string mk_rtsp_connection::m_strRtspMethod[] = RTSP_METHOD_STRING;

mk_rtsp_connection::mk_rtsp_connection()
{
    as_init_url(&m_url);
    m_url.port        = RTSP_DEFAULT_PORT;
    m_RecvBuf         = NULL;
    m_ulRecvSize      = 0;
    m_ulSeq           = 0;
    
    m_unSessionIndex  = 0;
    m_enPlayType      = PLAY_TYPE_LIVE;
    m_bSetUp          = false;
    m_sockHandle      = ACE_INVALID_HANDLE;
    m_pRtpSession     = NULL;
    m_pPeerSession    = NULL;
    m_pLastRtspMsg    = NULL;

    m_unSessionStatus  = RTSP_SESSION_STATUS_INIT;
    m_ulStatusTime     = SVS_GetSecondTime();

    m_strContentID     = "";
    m_bFirstSetupFlag  = true;
    m_strPlayRange     = "";
    m_lRedoTimerId     = -1;

    m_unTransType      = TRANS_PROTOCAL_UDP;
    m_cVideoInterleaveNum = 0;
    m_cAudioInterleaveNum = 0;
}

mk_rtsp_connection::~mk_rtsp_connection()
{
    m_unSessionIndex  = 0;
    m_sockHandle      = ACE_INVALID_HANDLE;
    m_pRtpSession     = NULL;
    m_pPeerSession    = NULL;
    m_pLastRtspMsg    = NULL;

    m_bFirstSetupFlag  = true;
    m_lRedoTimerId     = -1;
}


int32_t mk_rtsp_connection::open(const char* pszUrl)
{
    if(AS_ERROR_CODE_OK != as_parse_url(pszUrl,&m_url)) {
        return AS_ERROR_CODE_FAIL;
    }
    return AS_ERROR_CODE_OK;
}
int32_t mk_rtsp_connection::send_rtsp_request()
{
    CRtspOptionsMessage options;
    options.setCSeq(m_RtspProtocol.getCseq());
    options.setMsgType(RTSP_MSG_REQ);
    options.setRtspUrl((char*)&m_url.uri[0]);
    //options.setSession(m_strVideoSession);

    std::string strReq;
    if (RET_OK == options.encodeMessage(strReq)){
        AS_LOG(AS_LOG_WARNING,"options:rtsp client encode message fail.");
        return AS_ERROR_CODE_FAIL;
    }

    (void)m_RtspProtocol.saveSendReq(options.getCSeq(), options.getMethodType());

    if(AS_ERROR_CODE_OK != this->send(strReq.c_str(),strReq.length(),enSyncOp)) {
        AS_LOG(AS_LOG_WARNING,"options:rtsp client send message fail.");
        return AS_ERROR_CODE_FAIL;
    }
    setHandleRecv(AS_TRUE);
    return AS_ERROR_CODE_OK;
}

void mk_rtsp_connection::close()
{
    setHandleRecv(AS_FALSE);
    AS_LOG(AS_LOG_INFO,"close rtsp client.");
    return;
}
const char* mk_rtsp_connection::get_connect_addr()
{
    return (const char*)&m_url.host[0];
}
uint16_t    mk_rtsp_connection::get_connect_port()
{
    return m_url.port;
}

void  mk_rtsp_connection::set_rtp_over_tcp()
{
    return 
}

void  mk_rtsp_connection::set_status_callback(tsp_client_status cb,void* ctx)
{

}
void mk_rtsp_connection::handle_recv(void)
{
    as_network_addr peer;
    int32_t iRecvLen = (int32_t) (MAX_BYTES_PER_RECEIVE -  m_ulRecvSize);
    if (iRecvLen <= 0)
    {
        AS_LOG(AS_LOG_INFO,"rtsp connection,recv buffer is full, size[%u] length[%u].",
                MAX_BYTES_PER_RECEIVE,
                m_ulBufSize);
        return;
    }

    iRecvLen = this->recv(&m_RecvBuf[iRecvLen],&peer,iRecvLen,enAsyncOp);
    if (iRecvLen <= 0)
    {
        AS_LOG(AS_LOG_INFO,"rtsp connection recv data fail.");
        return;
    }

    m_ulRecvSize += iRecvLen;

    uint32_t processedSize = 0;
    uint32_t totalSize = m_ulRecvSize;
    int32_t nSize = 0;
    do
    {
        nSize = processRecvedMessage(&m_RecvBuf[processedSize],
                                     m_ulRecvSize - processedSize);
        if (nSize < 0) {
            AS_LOG(AS_LOG_WARNING,"tsp connection process recv data fail, close handle. ");
            return;
        }

        if (0 == nSize) {
            break;
        }

        processedSize += (uint32_t) nSize;
    }while (processedSize < totalSize);

    uint32_t dataSize = m_ulRecvSize - processedSize;
    if(0 < dataSize) {
        memmove(&m_RecvBuf[0],&m_RecvBuf[processedSize], dataSize);
    }
    m_ulRecvSize = dataSize;
    setHandleSend(AS_TRUE);
    return;
}
void mk_rtsp_connection::handle_send(void)
{
    setHandleSend(AS_FALSE);
}



int32_t mk_rtsp_connection::sendMessage(const char* pData, uint32_t unDataSize)
{
    if (NULL != m_pRtpSession)
    {
        return m_pRtpSession->sendMessage(pData, unDataSize,true);
    }

    ACE_Time_Value timeout(1);
    int32_t nSendSize = ACE::send_n(m_sockHandle, pData, unDataSize, &timeout);
    if (unDataSize != (uint32_t)nSendSize)
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send message fail, close handle[%d].",
                        m_unSessionIndex, m_sockHandle);
        return AS_ERROR_CODE_FAIL;
    }

    return AS_ERROR_CODE_OK;
}


int32_t mk_rtsp_connection::processRecvedMessage(const char* pData, uint32_t unDataSize)
{
    if ((NULL == pData) || (0 == unDataSize))
    {
        return AS_ERROR_CODE_FAIL;
    }

    if (RTSP_INTERLEAVE_FLAG == pData[0])
    {
        return handleRTPRTCPData(pData, unDataSize);
    }
    
    /* rtsp message */
    mk_rtsp_packet rtspPacket;
    uint32_t ulMsgLen  = 0;

    int32_t nRet = rtspPacket.checkRtsp(pData,unDataSize,ulMsgLen);

    if (AS_ERROR_CODE_OK != nRet)
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection check rtsp message fail.");
        return AS_ERROR_CODE_FAIL;
    }
    if(0 == ulMsgLen) {
        return 0; /* need more data deal */
    }

    nRet = rtspPacket.parse(pData,unDataSize);
    if (AS_ERROR_CODE_OK != nRet)
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection parser rtsp message fail.");
        return AS_ERROR_CODE_FAIL;
    }

    switch (rtspPacket.getMethodIndex())
    {
        case RtspDescribeMethod:
        {
            nRet = handleRtspDescribeReq(rtspPacket);
            break;
        }
        case RtspSetupMethod:
        {
            nRet = handleRtspSetupReq(rtspPacket);
            break;
        }        
        case RtspTeardownMethod:
        {
            nRet = handleRtspTeardownReq(rtspPacket);
            break;
        }
        case RtspPlayMethod:
        {
            nRet = handleRtspPlayReq(rtspPacket);
            break;
        }
        case RtspPauseMethod:
        {
            nRet = handleRtspPauseReq(rtspPacket);
            break;
        }
        case RtspOptionsMethod:
        {
            nRet = handleRtspOptionsReq(rtspPacket);
            break;
        }
        case RtspAnnounceMethod:
        {
            nRet = handleRtspOptionsReq(rtspPacket);
            break;
        }
        case RtspGetParameterMethod:
        {
            nRet = handleRtspGetParameterReq(rtspPacket);
            break;
        }
        case RtspSetParameterMethod:
        {
            nRet = handleRtspSetParameterReq(rtspPacket);
            break;
        }
        case RtspRedirectMethod:
        {
            nRet = handleRtspSetParameterReq(rtspPacket);
            break;
        }
        case RtspRecordMethod:
        {
            nRet = handleRtspSetParameterReq(rtspPacket);
            break;
        }
        case RtspResponseMethod:
        {
            break;
        }
        
        default:
        {
            break;
        }
    }
    AS_LOG(AS_LOG_INFO,"rtsp connection success to process rtsp message.");
    return ulMsgLen;
}

int32_t mk_rtsp_connection::handleRTPRTCPData(const char* pData, uint32_t unDataSize) const
{
    if (unDataSize < RTSP_INTERLEAVE_HEADER_LEN)
    {
        return 0;
    }
    uint32_t unMediaSize = (uint32_t) ACE_NTOHS(*(uint16_t*)(void*)&pData[2]);
    if (unDataSize - RTSP_INTERLEAVE_HEADER_LEN < unMediaSize)
    {
        return 0;
    }

    if (m_pRtpSession)
    {
        if (!m_pPeerSession)
        {
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle rtcp message fail, "
                    "can't find peer session.",m_unSessionIndex);

            return (int32_t)(unMediaSize + RTSP_INTERLEAVE_HEADER_LEN);
        }
        if(TRANS_PROTOCAL_TCP == m_unTransType)
        {
            if ((m_cVideoInterleaveNum == pData[1])
            || (m_cAudioInterleaveNum == pData[1]))
            {
                handleMediaData((const char*)(pData+RTSP_INTERLEAVE_HEADER_LEN),unMediaSize);
            }
        }

        STREAM_INNER_MSG innerMsg;
        fillStreamInnerMsg((char*)&innerMsg,
                        m_pRtpSession->getStreamId(),
                        NULL,
                        m_PeerAddr.get_ip_address(),
                        m_PeerAddr.get_port_number(),
                        INNER_MSG_RTCP,
                        0);
        (void)m_pRtpSession->handleInnerMessage(innerMsg, sizeof(innerMsg), *m_pPeerSession);
    }

    return (int32_t)(unMediaSize + RTSP_INTERLEAVE_HEADER_LEN);
}

void mk_rtsp_connection::handleMediaData(const char* pData, uint32_t unDataSize) const
{
    uint64_t ullRtpSessionId = 0;

    if(NULL == m_pRtpSession)
    {
        AS_LOG(AS_LOG_WARNING,"RtspPushSession,the rtp session is null.");
        return;
    }
    ullRtpSessionId = m_pRtpSession->getStreamId();

    ACE_Message_Block *pMsg = CMediaBlockBuffer::instance().allocMediaBlock();
    if (NULL == pMsg)
    {
        AS_LOG(AS_LOG_WARNING,"RtspPushSession alloc media block fail.");
        return ;
    }



    STREAM_TRANSMIT_PACKET *pPacket = (STREAM_TRANSMIT_PACKET *) (void*) pMsg->base();
    pMsg->wr_ptr(sizeof(STREAM_TRANSMIT_PACKET) - 1); //

    CRtpPacket rtpPacket;
    (void)rtpPacket.ParsePacket(pData ,unDataSize);

    pMsg->copy(pData, unDataSize);

    pPacket->PuStreamId = ullRtpSessionId;
    pPacket->enPacketType = STREAM_PACKET_TYPE_MEDIA_DATA;

    int32_t nRet = AS_ERROR_CODE_OK;
    nRet = CStreamMediaExchange::instance()->addData(pMsg);


    if (AS_ERROR_CODE_OK != nRet)
    {
        CMediaBlockBuffer::instance().freeMediaBlock(pMsg);

        return;
    }
    return;
}

int32_t mk_rtsp_connection::handleRtspMessage(mk_rtsp_message &rtspMessage)
{
    if (RTSP_MSG_REQ != rtspMessage.getMsgType())
    {
        if (RTSP_METHOD_ANNOUNCE != rtspMessage.getMethodType())
        {
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle not accepted method[%u].",
                              m_unSessionIndex, rtspMessage.getMethodType());
            return AS_ERROR_CODE_FAIL;
        }
    }

    int32_t nRet = AS_ERROR_CODE_OK;
    ACE_Guard<ACE_Recursive_Thread_Mutex> locker(m_RtspMutex);

    switch(rtspMessage.getMethodType())
    {
    case RTSP_METHOD_OPTIONS:
        nRet = handleRtspOptionsReq(rtspMessage);
        break;
    case RTSP_METHOD_DESCRIBE:
        nRet = handleRtspDescribeReq(rtspMessage);
        break;
    case RTSP_METHOD_SETUP:
        nRet = handleRtspSetupReq(rtspMessage);
        break;
    case RTSP_METHOD_PLAY:
        nRet = handleRtspPlayReq(rtspMessage);
        break;
    case RTSP_METHOD_PAUSE:
        nRet = handleRtspPauseReq(rtspMessage);
        break;
    case RTSP_METHOD_TEARDOWN:
        nRet = handleRtspTeardownReq(rtspMessage);
        break;
    case RTSP_METHOD_ANNOUNCE:
        nRet = handleRtspAnnounceReq(rtspMessage);
        break;
    case RTSP_METHOD_RECORD:
        nRet = handleRtspRecordReq(rtspMessage);
        break;
    case RTSP_METHOD_GETPARAMETER:
        nRet = handleRtspGetParameterReq(rtspMessage);
        break;
    default:
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle not accepted method[%u].",
                        m_unSessionIndex, rtspMessage.getMethodType());
        return AS_ERROR_CODE_FAIL;
    }
    return nRet;
}
int32_t mk_rtsp_connection::sendRtspOptionsReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_OPTIONS,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspDescribeReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_DESCRIBE,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspSetupReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_SETUP,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspPlayReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_PLAY,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspRecordReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_RECORD,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspGetParameterReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_GETPARAMETER,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspAnnounceReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_ANNOUNCE,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspPauseReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_PAUSE,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspTeardownReq()
{
    return sendRtspCmdWithContent(RTSP_METHOD_TEARDOWN,NULL,NULL,0);
}
int32_t mk_rtsp_connection::sendRtspCmdWithContent(RtspMethodType type,char* headstr,char* content,uint32_t lens)
{
    char message[MAX_RTSP_MSG_LEN] = {0};
    
    uint32_t ulHeadLen = 0;
    char*    start     = &message[ulHeadLen];
    uint32_t ulBufLen  = MAX_RTSP_MSG_LEN - ulHeadLen;
    
    /* first line */
    snprintf(start, ulBufLen, "%s %s RTSP/1.0\r\n"
                              "CSeq: %d\r\n"
                              "User-Agent: h.kernel\r\n", 
                              m_strRtspMethod[type].c_str(), &m_url.uri[0],m_ulSeq);
    ulHeadLen = strlen(message);
    start     = &message[ulHeadLen];
    ulBufLen  = MAX_RTSP_MSG_LEN - ulHeadLen;

    if(NULL != headstr) {
        snprintf(start, ulBufLen, "%s",headstr);
        ulHeadLen = strlen(message);
        start     = &message[ulHeadLen];
        ulBufLen  = MAX_RTSP_MSG_LEN - ulHeadLen;
    }
    /*
    if (rt->auth[0]) {
        char *str = ff_http_auth_create_response(&rt->auth_state,
                                                 rt->auth, url, method);
        if (str)
            av_strlcat(buf, str, sizeof(buf));
        av_free(str);
    }
    */

    /* content */
    if (lens > 0 && content) {
        snprintf(start, ulBufLen,"Content-Length: %d\r\n\r\n", lens);
        ulHeadLen = strlen(message);
        start     = &message[ulHeadLen];
        ulBufLen  = MAX_RTSP_MSG_LEN - ulHeadLen;
        memcpy(start,content,lens);
        ulHeadLen += lens;
    }
    else {
        snprintf(start, ulBufLen, "\r\n");
        ulHeadLen = strlen(message);
    }

    int nSendSize = this->send(&message[0],ulHeadLen,enSyncOp);
    if(nSendSize != ulHeadLen) {
        AS_LOG(AS_LOG_WARNING,"rtsp client send message fail,lens:[%d].",ulHeadLen);
        return AS_ERROR_CODE_FAIL;
    }


    m_CseqReqMap.insert(REQ_TYPE_MAP::value_type(m_ulSeq,type));
    m_ulSeq++；

    setHandleRecv(AS_TRUE);

    return AS_ERROR_CODE_OK;
}
int32_t mk_rtsp_connection::handleRtspResp()
{
    return AS_ERROR_CODE_OK;
}

int32_t mk_rtsp_connection::handleRtspOptionsReq(mk_rtsp_message &rtspMessage)
{
    CRtspOptionsMessage *pRequest = dynamic_cast<CRtspOptionsMessage*>(&rtspMessage);
    if (NULL == pRequest)
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle options request fail.");
        return AS_ERROR_CODE_FAIL;
    }


    pRequest->setRange(m_strPlayRange);
    pRequest->setStatusCode(RTSP_SUCCESS_OK);
    pRequest->setMsgType(RTSP_MSG_RSP);
    std::stringstream sessionIdex;
    sessionIdex << m_unSessionIndex;
    pRequest->setSession(sessionIdex.str());

    std::string strResp;
    if (AS_ERROR_CODE_OK != pRequest->encodeMessage(strResp))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection encode options response fail.");
        return AS_ERROR_CODE_FAIL;
    }

    if (AS_ERROR_CODE_OK != sendMessage(strResp.c_str(), strResp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send options response fail.");
        return AS_ERROR_CODE_FAIL;
    }

    AS_LOG(AS_LOG_INFO,"rtsp connection send options response success.");

    simulateSendRtcpMsg();

    return AS_ERROR_CODE_OK;
}



int32_t mk_rtsp_connection::handleRtspDescribeReq(const mk_rtsp_message &rtspMessage)
{
    //CSVSMediaLink MediaLink;
    if (RTSP_SESSION_STATUS_INIT != getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe req fail, status[%u] invalid.",
                m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }

    int32_t nRet = CSVSMediaLinkFactory::instance().parseMediaUrl(rtspMessage.getRtspUrl(),&m_pMediaLink);
    if((SVS_MEDIA_LINK_RESULT_SUCCESS != nRet)
        &&(SVS_MEDIA_LINK_RESULT_AUTH_FAIL != nRet))
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe req fail, content invalid.");
        return AS_ERROR_CODE_FAIL;
    }
    if(SVS_MEDIA_LINK_RESULT_AUTH_FAIL == nRet)
    {
        if(CStreamConfig::instance()->getUrlEffectiveWhile())
        {
            close();
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe req fail, auth invalid.",m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }
    }

    m_strContentID = m_pMediaLink.ContentID();

    if(NULL == m_pPeerSession)
    {
        m_pPeerSession = CStreamSessionFactory::instance()->findSession(m_strContentID);
        if ((NULL != m_pPeerSession)&&(PLAY_TYPE_AUDIO_LIVE == m_enPlayType))
        {
              CStreamSessionFactory::instance()->releaseSession(m_pPeerSession);
              m_pPeerSession = NULL;
              AS_LOG(AS_LOG_ERROR, "The Audio Request failed");
              return AS_ERROR_CODE_FAIL;
        }
    }
    
 
    if (NULL == m_pPeerSession)
    {
        CRtspDescribeMessage *pReq = new CRtspDescribeMessage();
        if (!pReq)  //lint !e774
        {
            return AS_ERROR_CODE_FAIL;
        }
        pReq->setRtspUrl(rtspMessage.getRtspUrl());
        pReq->setCSeq(rtspMessage.getCSeq());
        std::stringstream sessionIdex;
        sessionIdex << m_unSessionIndex;
        pReq->setSession(sessionIdex.str());

        if (!m_pLastRtspMsg)
        {
            delete m_pLastRtspMsg;
        }
        m_pLastRtspMsg = pReq;
        AS_LOG(AS_LOG_INFO,"rtsp session[%u] save describe request[%p].",
                        m_unSessionIndex, m_pLastRtspMsg);

        if (AS_ERROR_CODE_OK != sendMediaSetupReq(&m_pMediaLink))
        {
            delete m_pLastRtspMsg;
            m_pLastRtspMsg = NULL;

            AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe request fail, "
                    "send setup request fail.",
                    m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }

        return AS_ERROR_CODE_OK;
    }


    std::string strSdp;
    SDP_MEDIA_INFO  videoInfo, audioInfo;


    SDP_MEDIA_INFO stVideoInfo;
    SDP_MEDIA_INFO stAideoInfo;

    stVideoInfo.strControl   = "";
    stVideoInfo.strFmtp      = "";
    stVideoInfo.strRtpmap    = "";
    stVideoInfo.ucPayloadType= PT_TYPE_H264;
    stVideoInfo.usPort       = 0;

    stAideoInfo.strControl   = "";
    stAideoInfo.strFmtp      = "";
    stAideoInfo.strRtpmap    = "";
    stAideoInfo.ucPayloadType= PT_TYPE_PCMU;
    stAideoInfo.usPort       = 0;

     if ( (PLAY_TYPE_LIVE == m_enPlayType) || (PLAY_TYPE_FRONT_RECORD == m_enPlayType) || (PLAY_TYPE_PLAT_RECORD == m_enPlayType))
    {
        stVideoInfo.ucPayloadType = PT_TYPE_H264; /* PS -->H264 */
        m_RtspSdp.addVideoInfo(stVideoInfo);
        stVideoInfo.ucPayloadType = PT_TYPE_H265; /* PS -->H265 */
        m_RtspSdp.addVideoInfo(stVideoInfo);
        // stAideoInfo.ucPayloadType= PT_TYPE_PCMU;
        // m_RtspSdp.addAudioInfo(stAideoInfo);
        stAideoInfo.ucPayloadType= PT_TYPE_PCMA;
        m_RtspSdp.addAudioInfo(stAideoInfo);
    }
    else if ( PLAY_TYPE_AUDIO_LIVE == m_enPlayType)//�����GB28181Э�������PCMA,�����EHOME
    {
        
        stAideoInfo.ucPayloadType= PT_TYPE_PCMA;
        m_RtspSdp.addAudioInfo(stAideoInfo);
    }

    int32_t isplayback = 0;
    std::string strtimeRange="";
    if ( PLAY_TYPE_PLAT_RECORD == m_enPlayType)
    {
        isplayback = 1;
        if ("" == m_RtspSdp.getRange())
        {
            std::string strtmp = "";
            std::string strtmpUrl = m_RtspSdp.getUrl();
            std::string strStartTime = "";
            std::string strEndTime = "";
            strtmp += "range:npt=0-";
            uint32_t num = 0;
            num = getRange(strtmpUrl,strStartTime,strEndTime);

        if( 0 == num)
        {
            AS_LOG(AS_LOG_WARNING,"get timeRange fail,range in url is 0.");
            return AS_ERROR_CODE_FAIL;
        }

         std::stringstream stream;
         stream<<num;
         stream>>strtimeRange;
         strtmp += strtimeRange;
         AS_LOG(AS_LOG_WARNING,"time is = [%s]",strtmp.c_str());
         m_RtspSdp.setRange(strtmp);
        }
    }

    if (AS_ERROR_CODE_OK != m_RtspSdp.encodeSdp(strSdp,isplayback,strtimeRange,0,0))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection encode sdp info fail.");
        return AS_ERROR_CODE_FAIL;
    }


    CRtspDescribeMessage resp;
    resp.setMsgType(RTSP_MSG_RSP);
    resp.setCSeq(rtspMessage.getCSeq());
    resp.setStatusCode(RTSP_SUCCESS_OK);

    std::stringstream sessionIdex;
    sessionIdex << m_unSessionIndex;
    resp.setSession(sessionIdex.str());
    resp.setSdp(strSdp);

    std::string strResp;
    if (AS_ERROR_CODE_OK != resp.encodeMessage(strResp))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection encode describe response fail.");
        return AS_ERROR_CODE_FAIL;
    }

    if (AS_ERROR_CODE_OK != sendMessage(strResp.c_str(), strResp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send describe response fail.");
        return AS_ERROR_CODE_FAIL;
    }
    m_bSetUp = true;

    AS_LOG(AS_LOG_INFO,"rtsp connection handle describe request success.");
    return AS_ERROR_CODE_OK;
}

int32_t mk_rtsp_connection::handleRtspSetupReq(mk_rtsp_message &rtspMessage)
{
    std::string  strContentID;
    //CSVSMediaLink MediaLink;

    if (RTSP_SESSION_STATUS_SETUP < getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle setup req fail, status[%u] invalid.",
                m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }

    CRtspSetupMessage *pReq = dynamic_cast<CRtspSetupMessage*>(&rtspMessage);
    if (!pReq)  //lint !e774
    {
        return AS_ERROR_CODE_FAIL;
    }

    int32_t nRet = CSVSMediaLinkFactory::instance().parseMediaUrl(rtspMessage.getRtspUrl(),&m_pMediaLink);
    if((SVS_MEDIA_LINK_RESULT_SUCCESS != nRet)
        &&(SVS_MEDIA_LINK_RESULT_AUTH_FAIL != nRet))
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle setup req fail, content invalid.");
        return AS_ERROR_CODE_FAIL;
    }
    if((!m_bSetUp)&&(SVS_MEDIA_LINK_RESULT_AUTH_FAIL == nRet))
    {
        if(CStreamConfig::instance()->getUrlEffectiveWhile())
        {
            close();
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle setup req fail, auth invalid.",m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }
    }
    strContentID = m_pMediaLink.ContentID();

    if(NULL == m_pPeerSession)
    {
        m_pPeerSession = CStreamSessionFactory::instance()->findSession(m_strContentID);
    }

    if (NULL == m_pPeerSession)
    {
        CRtspSetupMessage *pSetupReq = new CRtspSetupMessage();
        if (!pSetupReq)  //lint !e774
        {
            return AS_ERROR_CODE_FAIL;
        }
        pSetupReq->setRtspUrl(rtspMessage.getRtspUrl());
        pSetupReq->setCSeq(rtspMessage.getCSeq());
        //pSetupReq->setSession(rtspMessage.getSession());
        std::stringstream sessionIdex;
        sessionIdex << m_unSessionIndex;
        pSetupReq->setSession(sessionIdex.str());
        pSetupReq->setTransType(pReq->getTransType());
        pSetupReq->setInterleaveNum(pReq->getInterleaveNum());
        pSetupReq->setClientPort(pReq->getClientPort());
        pSetupReq->setDestinationIp(pReq->getDestinationIp());

        if (!m_pLastRtspMsg)
        {
            delete m_pLastRtspMsg;
        }
        m_pLastRtspMsg = pSetupReq;

        if (AS_ERROR_CODE_OK != sendMediaSetupReq(&m_pMediaLink))
        {
            delete m_pLastRtspMsg;
            m_pLastRtspMsg = NULL;

            AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe request fail, "
                    "send setup request fail.",
                    m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }

        AS_LOG(AS_LOG_INFO,"rtsp session[%u] save setup request[%p], send media setup request to SCC.",
                        m_unSessionIndex, m_pLastRtspMsg);
        return AS_ERROR_CODE_OK;
    }


    if (!m_pRtpSession)
    {
        if (AS_ERROR_CODE_OK != createMediaSession())
        {
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle setup request fail, "
                    "create media session fail.",
                    m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }

        AS_LOG(AS_LOG_INFO,"rtsp connection create media session success.",
                        m_unSessionIndex);
    }

    pReq->setDestinationIp(m_PeerAddr.get_ip_address());

    std::stringstream sessionIdex;
    sessionIdex << m_unSessionIndex;
    pReq->setSession(sessionIdex.str());
    if (AS_ERROR_CODE_OK != m_pRtpSession->startStdRtpSession(*pReq))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection start media session fail.",
                                m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    ACE_INET_Addr addr;
    pReq->setMsgType(RTSP_MSG_RSP);
    pReq->setStatusCode(RTSP_SUCCESS_OK);
    m_unTransType = pReq->getTransType();
    if (TRANS_PROTOCAL_UDP == pReq->getTransType())
    {
        if (m_bFirstSetupFlag)
        {
            m_cVideoInterleaveNum = pReq->getInterleaveNum();
            addr.set(m_pRtpSession->getVideoAddr());
        }
        else
        {
            m_cAudioInterleaveNum = pReq->getInterleaveNum();
            addr.set(m_pRtpSession->getAudioAddr());
        }
        pReq->setServerPort(addr.get_port_number());
        pReq->setSourceIp(addr.get_ip_address());

        m_bFirstSetupFlag = false;
    }
    else
    {
        if (m_bFirstSetupFlag)
        {
            m_cVideoInterleaveNum = pReq->getInterleaveNum();
        }
        else
        {
            m_cAudioInterleaveNum = pReq->getInterleaveNum();
        }
        m_bFirstSetupFlag = false;
    }

    std::string strResp;
    if (AS_ERROR_CODE_OK != pReq->encodeMessage(strResp))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection encode setup response fail.",
                        m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    if (AS_ERROR_CODE_OK != sendMessage(strResp.c_str(), strResp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send setup response fail.",
                        m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    setStatus(RTSP_SESSION_STATUS_SETUP);
    AS_LOG(AS_LOG_INFO,"rtsp connection handle setup request success.",
                            m_unSessionIndex);
    return AS_ERROR_CODE_OK;
}
int32_t mk_rtsp_connection::handleRtspRecordReq(mk_rtsp_message &rtspMessage)
{
    if(PLAY_TYPE_AUDIO_LIVE == m_enPlayType)
    {
        AS_LOG(AS_LOG_INFO,"Receive Remote Client Audio Request OK!");
        if (STREAM_SESSION_STATUS_WAIT_CHANNEL_REDAY == m_pRtpSession->getStatus())
        {
            BUSINESS_LIST businessList;
            CStreamBusinessManager::instance()->findBusiness( m_pRtpSession->getStreamId(), businessList);
            for (BUSINESS_LIST_ITER iter = businessList.begin();
                    iter != businessList.end(); iter++)
            {
                CStreamBusiness *pBusiness = *iter;
                if (AS_ERROR_CODE_OK != pBusiness->start())
                {
                    CStreamBusinessManager::instance()->releaseBusiness(pBusiness);
                    AS_LOG(AS_LOG_WARNING,"start distribute fail, stream[%lld] start business fail.",
                                    m_pRtpSession->getStreamId());

                    return AS_ERROR_CODE_FAIL;
                }

                CStreamBusinessManager::instance()->releaseBusiness(pBusiness);
            }

            STREAM_INNER_MSG innerMsg;
            innerMsg.ullStreamID = m_pRtpSession->getStreamId();
            innerMsg.unBodyOffset = sizeof(STREAM_INNER_MSG);
            innerMsg.usMsgType = INNER_MSG_RTSP;

            (void) m_pRtpSession->handleInnerMessage(innerMsg,  sizeof(STREAM_INNER_MSG), *m_pPeerSession);

            sendMediaPlayReq();
        }

        if ((STREAM_SESSION_STATUS_DISPATCHING != m_pPeerSession->getStatus()))
        {
            return cacheRtspMessage(rtspMessage);
        }
        //sendMediaPlayReq();
        sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());

        setStatus(RTSP_SESSION_STATUS_PLAY);

        clearRtspCachedMessage();

        AS_LOG(AS_LOG_INFO,"rtsp connection handle rtsp play aduio request success.",
                        m_unSessionIndex);
        return  AS_ERROR_CODE_OK;
    }
    if (!m_pRtpSession)
    {
        return AS_ERROR_CODE_FAIL;
    }

    if (RTSP_SESSION_STATUS_SETUP > getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle record req fail, status[%u] invalid.",
                    m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }

    CRtspRecordMessage *pReq = dynamic_cast<CRtspRecordMessage*>(&rtspMessage);
    if (!pReq)
    {
        return AS_ERROR_CODE_FAIL;
    }

    sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());

    setStatus(RTSP_SESSION_STATUS_PLAY);

    clearRtspCachedMessage();

    AS_LOG(AS_LOG_INFO,"rtsp connection handle rtsp record request success.",
                        m_unSessionIndex);
    return AS_ERROR_CODE_OK;
}
int32_t mk_rtsp_connection::handleRtspGetParameterReq(mk_rtsp_message &rtspMessage)
{
    sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());

    AS_LOG(AS_LOG_INFO,"rtsp connection send get parameter response success.");

    simulateSendRtcpMsg();

    return AS_ERROR_CODE_OK;
}
int32_t mk_rtsp_connection::handleRtspSetParameterReq(mk_rtsp_message &rtspMessage)
{
    sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());

    AS_LOG(AS_LOG_INFO,"rtsp connection send get parameter response success.");

    simulateSendRtcpMsg();

    return AS_ERROR_CODE_OK;
}



int32_t mk_rtsp_connection::handleRtspPlayReq(mk_rtsp_message &rtspMessage)
{
    if (!m_pRtpSession)
    {
        return AS_ERROR_CODE_FAIL;
    }

    if (RTSP_SESSION_STATUS_SETUP > getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle play req fail, status[%u] invalid.",
                m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }

    CRtspPlayMessage *pReq = dynamic_cast<CRtspPlayMessage*>(&rtspMessage);
    if (!pReq)
    {
        return AS_ERROR_CODE_FAIL;
    }

    if (!m_pPeerSession)
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle rtsp play request fail, "
                "can't find peer session.",m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    if (STREAM_SESSION_STATUS_WAIT_CHANNEL_REDAY == m_pRtpSession->getStatus())
    {
        BUSINESS_LIST businessList;
        CStreamBusinessManager::instance()->findBusiness( m_pRtpSession->getStreamId(), businessList);
        for (BUSINESS_LIST_ITER iter = businessList.begin();
                iter != businessList.end(); iter++)
        {
            CStreamBusiness *pBusiness = *iter;
            if (AS_ERROR_CODE_OK != pBusiness->start())
            {
                CStreamBusinessManager::instance()->releaseBusiness(pBusiness);
                AS_LOG(AS_LOG_WARNING,"start distribute fail, stream[%lld] start business fail.",
                                m_pRtpSession->getStreamId());

                return AS_ERROR_CODE_FAIL;
            }

            CStreamBusinessManager::instance()->releaseBusiness(pBusiness);
        }

        STREAM_INNER_MSG innerMsg;
        innerMsg.ullStreamID = m_pRtpSession->getStreamId();
        innerMsg.unBodyOffset = sizeof(STREAM_INNER_MSG);
        innerMsg.usMsgType = INNER_MSG_RTSP;

        (void) m_pRtpSession->handleInnerMessage(innerMsg,  sizeof(STREAM_INNER_MSG), *m_pPeerSession);

        if (!pReq->hasRange())
        {
            MEDIA_RANGE_S   stRange;
            stRange.enRangeType = RANGE_TYPE_NPT;
            stRange.MediaBeginOffset = OFFSET_CUR;
            stRange.MediaEndOffset = OFFSET_END;

            pReq->setRange(stRange);
        }
        else
        {
            pReq->getRange(m_strPlayRange);
        }
    }

    if ((STREAM_SESSION_STATUS_DISPATCHING != m_pPeerSession->getStatus()))
    {
        return cacheRtspMessage(rtspMessage);
    }


    sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());


    setStatus(RTSP_SESSION_STATUS_PLAY);

    clearRtspCachedMessage();

    //send the play request
    if(m_bSetUp){
        sendMediaPlayReq();
    }
    else
    {
        /* Key Frame request */
        sendKeyFrameReq();
    }
    AS_LOG(AS_LOG_INFO,"rtsp connection handle rtsp play request success.",
                    m_unSessionIndex);
    return AS_ERROR_CODE_OK;
}


int32_t mk_rtsp_connection::handleRtspAnnounceReq(const mk_rtsp_message &rtspMessage)
{
    if (RTSP_SESSION_STATUS_INIT != getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle describe req fail, status[%u] invalid.",
                    m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }
    //CSVSMediaLink MediaLink;

    int32_t nRet = CSVSMediaLinkFactory::instance().parseMediaUrl(rtspMessage.getRtspUrl(),&m_pMediaLink);
    if((SVS_MEDIA_LINK_RESULT_SUCCESS != nRet)
        &&(SVS_MEDIA_LINK_RESULT_AUTH_FAIL != nRet))
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle announce req fail, content invalid.");
        return AS_ERROR_CODE_FAIL;
    }
    if(SVS_MEDIA_LINK_RESULT_AUTH_FAIL == nRet)
    {
        if(CStreamConfig::instance()->getUrlEffectiveWhile())
        {
            close();
            AS_LOG(AS_LOG_WARNING,"rtsp connection handle announce req fail, auth invalid.",m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }
    }

    if(NULL == m_pPeerSession)
    {
        m_pPeerSession = CStreamSessionFactory::instance()->findSession(m_strContentID);
    }

    if (NULL == m_pPeerSession)
    {
        CRtspAnnounceMessage *pReq = new CRtspAnnounceMessage();
        if (!pReq)  //lint !e774
        {
            return AS_ERROR_CODE_FAIL;
        }
        pReq->setRtspUrl(rtspMessage.getRtspUrl());
        pReq->setCSeq(rtspMessage.getCSeq());
        //pReq->setSession(rtspMessage.getSession());
        std::stringstream sessionIdex;
        sessionIdex << m_unSessionIndex;
        pReq->setSession(sessionIdex.str());
        std::string strContendType = rtspMessage.getContetType();
        std::string strContend = rtspMessage.getBody();
        pReq->setBody(strContendType,strContend);

        if (!m_pLastRtspMsg)
        {
            delete m_pLastRtspMsg;
        }
        m_pLastRtspMsg = pReq;
        AS_LOG(AS_LOG_INFO,"rtsp session[%u] save Announce request[%p].",
                            m_unSessionIndex, m_pLastRtspMsg);

        if (AS_ERROR_CODE_OK != sendMediaSetupReq(&m_pMediaLink))
        {
            delete m_pLastRtspMsg;
            m_pLastRtspMsg = NULL;

            AS_LOG(AS_LOG_WARNING,"rtsp connection handle Announce request fail, "
                        "send setup request fail.",
                        m_unSessionIndex);
            return AS_ERROR_CODE_FAIL;
        }

        return AS_ERROR_CODE_OK;
    }

    std::string strSdp = rtspMessage.getBody();
    if (AS_ERROR_CODE_OK == m_RtspSdp.decodeSdp(strSdp))
    {
        m_RtspSdp.setUrl(rtspMessage.getRtspUrl());
    }

    sendRtspResp(RTSP_SUCCESS_OK, rtspMessage.getCSeq());

    AS_LOG(AS_LOG_INFO,"rtsp connection handle describe request success.");
    return AS_ERROR_CODE_OK;
}


int32_t mk_rtsp_connection::handleRtspPauseReq(mk_rtsp_message &rtspMessage)
{
    if (RTSP_SESSION_STATUS_PLAY != getStatus()
            && RTSP_SESSION_STATUS_PAUSE != getStatus())
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle pause req fail, status[%u] invalid.",
                m_unSessionIndex, getStatus());
        return AS_ERROR_CODE_FAIL;
    }

    CRtspPauseMessage *pReq = dynamic_cast<CRtspPauseMessage*>(&rtspMessage);
    if (!pReq)
    {
        return AS_ERROR_CODE_FAIL;
    }

    if (!m_pPeerSession)
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle rtsp play request fail, "
                "can't find peer session.",m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    std::string strRtsp;
    (void)pReq->encodeMessage(strRtsp);

    mk_rtsp_packet rtspPack;
    if (0 != rtspPack.parse(strRtsp.c_str(), strRtsp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle rtsp pause request fail, "
                        "parse rtsp packet fail.",
                        m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    if (AS_ERROR_CODE_OK != m_pPeerSession->sendVcrMessage(rtspPack))
    {
        sendRtspResp(RTSP_SERVER_INTERNAL, rtspMessage.getCSeq());
        AS_LOG(AS_LOG_WARNING,"rtsp connection handle rtsp pause request fail, "
                "peer session send vcr message fail.",
                m_unSessionIndex);
        return AS_ERROR_CODE_FAIL;
    }

    setStatus(RTSP_SESSION_STATUS_PAUSE);
    AS_LOG(AS_LOG_INFO,"rtsp connection handle rtsp pause request success.",
                     m_unSessionIndex);
    return AS_ERROR_CODE_OK;
}

// TEARDOWN
int32_t mk_rtsp_connection::handleRtspTeardownReq(mk_rtsp_message &rtspMessage)
{
    rtspMessage.setMsgType(RTSP_MSG_RSP);
    rtspMessage.setStatusCode(RTSP_SUCCESS_OK);

    std::string strRtsp;
    if (AS_ERROR_CODE_OK != rtspMessage.encodeMessage(strRtsp))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection encode teardown response fail.");
    }

    if (AS_ERROR_CODE_OK != sendMessage(strRtsp.c_str(), strRtsp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send teardown response fail.");
    }

    //close the session
    (void)handle_close(m_sockHandle, 0);
    setStatus(RTSP_SESSION_STATUS_TEARDOWN);

    AS_LOG(AS_LOG_INFO,"rtsp connection handle rtsp teardown request success.",
                     m_unSessionIndex);
    return AS_ERROR_CODE_OK;
}

void mk_rtsp_connection::sendRtspResp(uint32_t unStatusCode, uint32_t unCseq)
{
    std::string strResp;
    mk_rtsp_message::encodeCommonResp(unStatusCode, unCseq, strResp);

    if (AS_ERROR_CODE_OK != sendMessage(strResp.c_str(), strResp.length()))
    {
        AS_LOG(AS_LOG_WARNING,"rtsp connection send common response fail.");
    }
    else
    {
        AS_LOG(AS_LOG_INFO,"rtsp connection send common response success.");
    }

    AS_LOG(AS_LOG_DEBUG,"%s", strResp.c_str());
    return;
}

void mk_rtsp_connection::trimString(std::string& srcString) const
{
    string::size_type pos = srcString.find_last_not_of(' ');
    if (pos != string::npos)
    {
        (void) srcString.erase(pos + 1);
        pos = srcString.find_first_not_of(' ');
        if (pos != string::npos)
            (void) srcString.erase(0, pos);
    }
    else
        (void) srcString.erase(srcString.begin(), srcString.end());

    return;
}


/*******************************************************************************************************
 * 
 * 
 *
 * ****************************************************************************************************/

mk_rtsp_server::mk_rtsp_server()
{
    m_cb  = NULL;
    m_ctx = NULL;
}

mk_rtsp_server::~mk_rtsp_server()
{
}
void mk_rtsp_server::set_callback(rtsp_server_request cb,void* ctx)
{
    m_cb = cb;
    m_ctx = ctx;
}
long mk_rtsp_server::handle_accept(const as_network_addr *pRemoteAddr, as_tcp_conn_handle *&pTcpConnHandle)
{
    
}