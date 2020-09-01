#ifndef __MK_RTSP_RTP_FRAME_ORGANIZER_INCLUDE_H__
#define __MK_RTSP_RTP_FRAME_ORGANIZER_INCLUDE_H__

#include <deque>
#include <list>
#include <map>
#include "as.h"

typedef enum
{
    RTP_H264_NALU_TYPE_UNDEFINED    = 0,
    RTP_H264_NALU_TYPE_IDR          = 5,
    RTP_H264_NALU_TYPE_SEI          = 6,
    RTP_H264_NALU_TYPE_SPS          = 7,
    RTP_H264_NALU_TYPE_PPS          = 8,
    RTP_H264_NALU_TYPE_STAP_A       = 24,
    RTP_H264_NALU_TYPE_STAP_B       = 25,
    RTP_H264_NALU_TYPE_MTAP16       = 26,
    RTP_H264_NALU_TYPE_MTAP24       = 27,
    RTP_H264_NALU_TYPE_FU_A         = 28,
    RTP_H264_NALU_TYPE_FU_B         = 29,
    RTP_H264_NALU_TYPE_END
}RTP_H264_NALU_TYPE;

typedef struct
{
    //byte 0
    uint8_t TYPE:5;
    uint8_t NRI:2;
    uint8_t F:1;
}RTP_H264_FU_INDICATOR; /**//* 1 BYTES */

#define MAX_RTP_FRAME_CACHE_NUM     5
#define MAX_RTP_SEQ                 65535

typedef struct _stRTP_PACK_INFO_S
{
    uint16_t      usSeq;
    uint32_t      unTimestamp;
    bool          bMarker;
    char*         pRtpMsgBlock;
    uint32_t      len;
}RTP_PACK_INFO_S;
typedef std::deque<RTP_PACK_INFO_S>       RTP_PACK_QUEUE;
typedef struct _stRTP_FRAME_INFO_S
{
    uint32_t        unTimestamp;
    bool            bMarker;
    RTP_PACK_QUEUE  PacketQueue;
}RTP_FRAME_INFO_S;
typedef std::map<uint32_t,RTP_FRAME_INFO_S*> RTP_FRAME_MAP_S;
typedef std::list<RTP_FRAME_INFO_S*> RTP_FRAME_LIST_S;


#define INVALID_RTP_SEQ     (0x80000000)

class mk_rtp_frame_handler
{
public:
    mk_rtp_frame_handler(){}

    virtual ~mk_rtp_frame_handler(){}

    virtual void handleRtpFrame(RTP_PACK_QUEUE &rtpFrameList) = 0;
};

class mk_rtp_frame_organizer
{
public:
    mk_rtp_frame_organizer();
    virtual ~mk_rtp_frame_organizer();

    int32_t init(mk_rtp_frame_handler* pHandler, uint32_t unMaxFrameCache = MAX_RTP_FRAME_CACHE_NUM);

    int32_t insertRtpPacket(char* pRtpBlock,uint32_t len);

    void release();
private:
    int32_t insert(RTP_FRAME_INFO_S *pFrameinfo,const RTP_PACK_INFO_S &info);

    int32_t insertRange(RTP_FRAME_INFO_S *pFrameinfo ,const RTP_PACK_INFO_S &info);

    void checkFrame();

    void handleFinishedFrame(RTP_FRAME_INFO_S *pFrameinfo);

    void releaseRtpPacket(RTP_FRAME_INFO_S *pFrameinfo);
    RTP_FRAME_INFO_S* InsertFrame(uint32_t  unTimestamp);
    RTP_FRAME_INFO_S* GetSmallFrame();
private:
    uint32_t                 m_unMaxCacheFrameNum;
    mk_rtp_frame_handler*        m_pRtpFrameHandler;

    RTP_FRAME_MAP_S          m_RtpFrameMap;
    RTP_FRAME_LIST_S         m_RtpFrameFreeList;
};

#endif /* __MK_RTSP_RTP_FRAME_ORGANIZER_INCLUDE_H__ */