#include "mk_rtmp_connection.h"
#include "mk_media_service.h"



mk_rtmp_connection::mk_rtmp_connection()
{
    m_rtmpHandle = NULL;
}
mk_rtmp_connection::~mk_rtmp_connection()
{
    if(NULL != m_rtmpHandle) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
    }
}
int32_t mk_rtmp_connection::start(const char* pszUrl)
{
    m_rtmpHandle = srs_rtmp_create(pszUrl);
    if(NULL == m_rtmpHandle) {
        return AS_ERROR_CODE_FAIL;
    }

    if(AS_ERROR_CODE_OK != srs_rtmp_connect_app(m_rtmpHandle)) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
        return AS_ERROR_CODE_FAIL;
    }
    
    if(AS_ERROR_CODE_OK != srs_rtmp_play_stream(m_rtmpHandle)) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
        return AS_ERROR_CODE_FAIL;
    }

    SOCKET Socket = srs_rtmp_get_socket(m_rtmpHandle);
    
    if(SRS_INVALID_FD == Socket) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
        return AS_ERROR_CODE_FAIL;
    }
    /* register the network service */
    as_network_svr* pNetWork = mk_media_service::instance().get_client_network_svr(this);
    
    if(AS_ERROR_CODE_OK != pNetWork->regTcpMonitorClient(this,Socket)) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
        return AS_ERROR_CODE_FAIL;
    }

    return AS_ERROR_CODE_OK;
}
void    mk_rtmp_connection::stop()
{
    if(NULL != m_rtmpHandle) {
        srs_rtmp_destroy(m_rtmpHandle);
        m_rtmpHandle = NULL;
    }
    /* unregister the network service */
    as_network_svr* pNetWork = mk_media_service::instance().get_client_network_svr(this);
    pNetWork->removeTcpClient(this);
    return;
}
int32_t mk_rtmp_connection::recv_next()
{
    as_handle::setHandleRecv(AS_TRUE);
}
void mk_rtmp_connection::handle_recv(void)
{
    int size;
    char type;
    char* data;
    uint32_t timestamp, pts;
    MR_MEDIA_TYPE enType = MR_MEDIA_TYPE_INVALID;

    if(NULL == m_rtmpHandle) {
        return;
    }
    
        
    if (srs_rtmp_read_packet(m_rtmpHandle, &type, &timestamp, &data, &size) != 0) {
        return;
    }

    if (srs_utils_parse_timestamp(timestamp, type, data, size, &pts) != 0) {
        return;
    }

    if(m_ulRecvBufLen < size) {
        return;
    }
    

    if (type == SRS_RTMP_TYPE_VIDEO) {      
		/* SrsCodecVideoAVC                     = 7,*/
		if (srs_utils_flv_video_codec_id(data, size) != 7) {
			return;// Jus support H264
        }
        enType = MR_MEDIA_TYPE_H264;
	}
	else {
        /**
        * get the SoundFormat of audio tag.
        * Format of SoundData. The following values are defined:
        *               0 = Linear PCM, platform endian
        *               1 = ADPCM
        *               2 = MP3
        *               3 = Linear PCM, little endian
        *               4 = Nellymoser 16 kHz mono
        *               5 = Nellymoser 8 kHz mono
        *               6 = Nellymoser
        *               7 = G.711 A-law logarithmic PCM
        *               8 = G.711 mu-law logarithmic PCM
        *               9 = reserved
        *               10 = AAC
        *               11 = Speex
        *               14 = MP3 8 kHz
        *               15 = Device-specific sound
        *               Formats 7, 8, 14, and 15 are reserved.
        *               AAC is supported in Flash Player 9,0,115,0 and higher.
        *               Speex is supported in Flash Player 10 and higher.
        * @return the sound format. -1(0xff) for error.
        */
        char format = srs_utils_flv_audio_sound_format(data, size);
        if(7 == format) {
            enType = MR_MEDIA_TYPE_G711A;
        }
        else if(8 == format) {
            enType = MR_MEDIA_TYPE_G711U;
        }
        else {
            return;
        }
	}
    memcpy(m_recvBuf,data,size);
    m_ulRecvLen = size;
    handle_connection_media(enType,pts);
    free(data);
    return;
}
void mk_rtmp_connection::handle_send(void)
{
    as_handle::setHandleSend(AS_FALSE);
    return;
}