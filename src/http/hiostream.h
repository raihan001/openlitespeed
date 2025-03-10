/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2018  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/
#ifndef HIOSTREAM_H
#define HIOSTREAM_H


#include <sys/types.h>
#include <edio/inputstream.h>
#include <edio/outputstream.h>
#include <log4cxx/logsession.h>
#include <lsdef.h>
#include <lsr/ls_types.h>

class IOVec;

class Aiosfcb;
class HioHandler;
class HttpRespHeaders;
class HioChainStream;
class NtwkIOLink;
class ClientInfo;
class HioCrypto;
class ServerAddrInfo;
class UnpackedHeaders;

enum HioState
{
    HIOS_DISCONNECTED,
    HIOS_CONNECTED,
    HIOS_CLOSING,
    HIOS_SHUTDOWN,
    HIOS_RESET
};

enum HiosProtocol
{
    HIOS_PROTO_HTTP  = 0,
    HIOS_PROTO_SPDY2 = 1,
    HIOS_PROTO_SPDY3 = 2,
    HIOS_PROTO_SPDY31 = 3,
    HIOS_PROTO_HTTP2 = 4,
    HIOS_PROTO_QUIC = 5,
    HIOS_PROTO_MAX
};

#define HIO_FLAG_PEER_SHUTDOWN      (1<<0)
#define HIO_FLAG_LOCAL_SHUTDOWN     (1<<1)
#define HIO_FLAG_WANT_READ          (1<<2)
#define HIO_FLAG_WANT_WRITE         (1<<3)
#define HIO_FLAG_ABORT              (1<<4)
#define HIO_FLAG_PEER_RESET         (1<<5)
#define HIO_FLAG_HANDLER_RELEASE    (1<<6)
#define HIO_FLAG_PAUSE_WRITE        (1<<7)
#define HIO_FLAG_FLOWCTRL           (1<<8)
#define HIO_FLAG_BLACK_HOLE         (1<<9)
#define HIO_FLAG_PASS_THROUGH       (1<<10)
#define HIO_FLAG_PASS_SETCOOKIE     (1<<11)
#define HIO_FLAG_FROM_LOCAL         (1<<12)
#define HIO_FLAG_PUSH_CAPABLE       (1<<13)
#define HIO_FLAG_INIT_SESS          (1<<14)
#define HIO_FLAG_IS_PUSH            (1<<15)
#define HIO_FLAG_WRITE_BUFFER       (1<<16)
#define HIO_FLAG_SENDFILE           (1<<17)
#define HIO_FLAG_DELAY_FLUSH        (1<<18)
#define HIO_FLAG_PRI_SET            (1<<19)
#define HIO_FLAG_ALTSVC_SENT        (1<<20)


#define HIO_EOR                     1

#define HIO_PRIORITY_HIGHEST        (0)
#define HIO_PRIORITY_LOWEST         (7)
#define HIO_PRIORITY_HTML           (2)
#define HIO_PRIORITY_CSS            (HIO_PRIORITY_HTML + 1)
#define HIO_PRIORITY_JS             (HIO_PRIORITY_CSS + 1)
#define HIO_PRIORITY_IMAGE          (HIO_PRIORITY_JS + 1)
#define HIO_PRIORITY_DOWNLOAD       (HIO_PRIORITY_IMAGE + 1)
#define HIO_PRIORITY_PUSH           (HIO_PRIORITY_DOWNLOAD + 1)
#define HIO_PRIORITY_LARGEFILE      (HIO_PRIORITY_LOWEST)


struct ConnInfo
{
    ClientInfo             *m_pClientInfo;
    union
    {
        HioCrypto          *m_pCrypto;
        struct ssl_st      *m_pSsl;
    };
    const ServerAddrInfo   *m_pServerAddrInfo;
    unsigned int            m_remotePort;
};


class HioStream : public InputStream, public OutputStream,
    public LogSession
{

public:
    HioStream()
    {
        LS_ZERO_FILL(m_pHandler, m_tmLastActive);
    }
    virtual ~HioStream();

    virtual int sendfile(int fdSrc, off_t off, size_t size, int flag) = 0;
    virtual int aiosendfile(Aiosfcb *cb)
    {   return 0;   }
    virtual int aiosendfiledone(Aiosfcb *cb)
    {   return 0;   }
    virtual int readv(struct iovec *vector, int count)
    {       return -1;      }
    virtual int write(const char *buf, int len) = 0;
    virtual int write(const char *buf, int len, int flag)
    {       return write(buf, len);     }

    virtual int sendRespHeaders(HttpRespHeaders *pHeaders, int isNoBody) = 0;

    virtual int  shutdown() = 0;
    virtual void suspendRead()  = 0;
    virtual void continueRead() = 0;
    virtual void suspendWrite() = 0;
    virtual void continueWrite() = 0;
    virtual void switchWriteToRead() = 0;
    virtual int  onTimer()             {    return 0;   }
    virtual void suspendEventNotify()  {};
    virtual void resumeEventNotify()   {};
    //virtual SslConnection * getSSL() = 0;

    //virtual uint32_t GetStreamID() = 0;
    virtual int detectClose()       {   return 0;   }

    void reset(int32_t timeStamp)
    {
        memset(&m_pHandler, 0, (char *)(&m_iFlag + 1) - (char *)&m_pHandler);
        m_tmLastActive = timeStamp;
    }

    void setClientInfo(ClientInfo *p)   {   m_connInfo.m_pClientInfo = p;      }
    ClientInfo *getClientInfo() const   {   return m_connInfo.m_pClientInfo;   }

    void setConnInfo(const ConnInfo *p)
    {   memmove(&m_connInfo, p, sizeof(m_connInfo));        }
    const ConnInfo *getConnInfo() const {   return &m_connInfo;     }

    int getPriority() const     {   return m_iPriority;     }
    void setPriority(int pri)
    {
        if (pri > HIO_PRIORITY_LOWEST)
            pri = HIO_PRIORITY_LOWEST;
        else if (pri < HIO_PRIORITY_HIGHEST)
            pri = HIO_PRIORITY_HIGHEST;
        m_iPriority = pri;
    }
    void raisePriority(int by = 1)
    {   setPriority(m_iPriority - by);  }
    void lowerPriority(int by = 1)
    {   setPriority(m_iPriority + by);  }

    int isSendfileAvail() const
    {   return m_iFlag & HIO_FLAG_SENDFILE;     }

    int isFromLocalAddr() const
    {   return m_iFlag & HIO_FLAG_FROM_LOCAL;   }

    HioHandler *getHandler() const  {   return m_pHandler;  }
    void setHandler(HioHandler *p)  {   m_pHandler = p;     }
    void switchHandler(HioHandler *pCurrent, HioHandler *pNew);

    void wantRead(int want)
    {
        if (!(m_iFlag & HIO_FLAG_WANT_READ) == !want)
            return;
        if (want)
        {
            m_iFlag |= HIO_FLAG_WANT_READ;
            continueRead();
        }
        else
        {
            m_iFlag &= ~HIO_FLAG_WANT_READ;
            suspendRead();
        }
    }
    void wantWrite(int want)
    {
        if (!(m_iFlag & HIO_FLAG_WANT_WRITE) == !want)
            return;
        if (want)
        {
            m_iFlag |= HIO_FLAG_WANT_WRITE;
            continueWrite();
        }
        else
        {
            m_iFlag &= ~HIO_FLAG_WANT_WRITE;
            suspendWrite();
        }
    }
    short isWantRead() const    {   return m_iFlag & HIO_FLAG_WANT_READ;    }
    short isWantWrite() const   {   return m_iFlag & HIO_FLAG_WANT_WRITE;   }
    short isReadyToRelease() const {    return m_iFlag & HIO_FLAG_HANDLER_RELEASE;  }

    void setFlag(uint32_t flagbit, int val)
    {   m_iFlag = (val) ? (m_iFlag | flagbit) : (m_iFlag & ~flagbit);       }
    uint32_t getFlag(uint32_t flagbit) const {   return flagbit & m_iFlag;  }
    uint32_t getFlag() const                 {   return m_iFlag;            }

    bool isAborted() const      {   return m_iFlag & HIO_FLAG_ABORT;        }
    void setAbortedFlag()       {   m_iFlag |= HIO_FLAG_ABORT;              }

    bool isClosing() const      {   return m_iState != HIOS_CONNECTED;      }

    void handlerReadyToRelease() {   m_iFlag |= HIO_FLAG_HANDLER_RELEASE;    }
    bool isPauseWrite()  const   {   return m_iFlag & HIO_FLAG_PAUSE_WRITE;  }

    char  getProtocol() const   {   return m_iProtocol;     }
    void  setProtocol(int p)    {   m_iProtocol = p;        }

    int   isSpdy() const        {   return m_iProtocol;     }
    bool  isHttp2() const       {   return m_iProtocol == HIOS_PROTO_HTTP2; }

    char  getState() const      {   return m_iState;        }
    void  setState(HioState st) {   m_iState = st;          }

    void  bytesRecv(int n)      {   m_lBytesRecv += n;      }
    void  bytesSent(int n)      {   m_lBytesSent += n;      }

    off_t getBytesRecv() const  {   return m_lBytesRecv;    }
    off_t getBytesSent() const  {   return m_lBytesSent;    }
    off_t getBytesTotal() const {   return m_lBytesRecv + m_lBytesSent; }

    void resetBytesCount()
    {
        m_lBytesRecv = 0;
        m_lBytesSent = 0;
    }

    virtual int push(ls_str_t *pUrl, ls_str_t *pHost,
                     ls_strpair_t *pHeaders)
    {   return -1;      }

    void setActiveTime(uint32_t lTime)
    {   m_tmLastActive = lTime;              }
    uint32_t getActiveTime() const
    {   return m_tmLastActive;               }

    void onPeerClose()
    {
        m_iFlag |= HIO_FLAG_PEER_SHUTDOWN;
        if (m_pHandler)
            handlerOnClose();
        close();
    }

    void tobeClosed()
    {
        if (m_iState < HIOS_SHUTDOWN)
            m_iState = HIOS_CLOSING;
    }

    short isPeerShutdown() const {  return m_iFlag & HIO_FLAG_PEER_SHUTDOWN;    }

    const ls_str_t *getProtocolName() const
    {   return getProtocolName((HiosProtocol)m_iProtocol);   }

    static const ls_str_t *getProtocolName(HiosProtocol proto);

    virtual UnpackedHeaders *getReqHeaders()
    {   return NULL;    }

    bool isWriteBuffer() const   {   return m_iFlag & HIO_FLAG_WRITE_BUFFER; }
    virtual ssize_t bufferedWrite(const char *data, size_t size)
    {   return 0;   }
protected:
    void releaseHandler();

private:

    HioHandler         *m_pHandler;
    ConnInfo            m_connInfo;
    off_t               m_lBytesRecv;
    off_t               m_lBytesSent;
    char                m_iState;
    char                m_iProtocol;
    short               m_iPriority;
    uint32_t            m_iFlag;
    uint32_t            m_tmLastActive;


    HioStream(const HioStream &other);
    HioStream &operator=(const HioStream &other);
    bool operator==(const HioStream &other) const;

    void handlerOnClose();

};

class HioHandler
{
    HioStream *m_pStream;

public:
    HioHandler()
        : m_pStream(NULL)
    {}
    virtual ~HioHandler();

    HioStream *getStream() const           {   return m_pStream;   }
    void attachStream(HioStream *p)
    {
        m_pStream  = p;
        p->setHandler(this);
    }

    HioStream *detachStream()
    {
        HioStream *pStream = m_pStream;
        if (pStream)
        {
            m_pStream = NULL;
            if (pStream->getHandler() == this)
                pStream->setHandler(NULL);
        }
        return pStream;
    }

    virtual int onInitConnected() = 0;
    virtual int onReadEx()  = 0;
    virtual int onWriteEx() = 0;
    virtual int onCloseEx() = 0;
    virtual int onTimerEx() = 0;

    virtual void recycle() = 0;

    virtual int h2cUpgrade(HioHandler *pOld, const char * pBuf, int size);
    virtual int detectContentLenMismatch(int buffered)  {   return 0;  }

private:
    HioHandler(const HioHandler &other);
    HioHandler &operator=(const HioHandler &other);
    bool operator==(const HioHandler &other) const;
};


inline void HioStream::releaseHandler()
{
    if (m_pHandler)
    {
        m_pHandler->recycle();
        m_pHandler = NULL;
    }
}


#endif // HIOSTREAM_H
