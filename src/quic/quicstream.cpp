/*
 *
 */

#include "quicstream.h"
#include "quiclog.h"
#include <lsquic.h>
#include <lsr/ls_strtool.h>
#include <lsr/ls_str.h>
#include <util/datetime.h>
#include <http/clientinfo.h>
#include <http/hiohandlerfactory.h>
#include <http/httpstatuscode.h>
#include <http/httprespheaders.h>
#include <log4cxx/logger.h>
#include <lsr/ls_swap.h>

#include <inttypes.h>
#include <errno.h>
#include <stdio.h>


QuicStream::~QuicStream()
{
    lsquic_stream_ctx_t::m_pStream = NULL;
}


int QuicStream::init(lsquic_stream_t *s)
{
    m_pStream = s;
    setActiveTime(DateTime::s_curTime);
    clearLogId();
    setProtocol(HIOS_PROTO_QUIC);

    int flag = HIO_FLAG_FLOWCTRL;
    /* Turn on the push capable flag: check it when push() is called and
     * unset it if necessary.
     */
    if (!lsquic_stream_is_pushed(m_pStream))
        flag |= HIO_FLAG_PUSH_CAPABLE;
    setFlag(flag, 1);

    setState(HIOS_CONNECTED);

    int pri = HIO_PRIORITY_HTML;
//     if (pPriority)
//     {
//         if (pPriority->m_weight <= 32)
//             pri = (32 - pPriority->m_weight) >> 2;
//         else
//             pri = (256 - pPriority->m_weight) >> 5;
//     }
    setPriority(pri);

    LS_DBG_L(this, "QuicStream::init(), id: %" PRIu64 ", priority: %d, flag: %d. ",
             lsquic_stream_id(s), pri, (int)getFlag());
    return 0;

}


int QuicStream::processUpkdHdrs(QuicUpkdHdrs *hdrs)
{
    HioHandler *pHandler = HioHandlerFactory::getHandler(HIOS_PROTO_HTTP);
    if (!pHandler)
        return LS_FAIL;

    pHandler->attachStream(this);

    m_pHeaders = &hdrs->headers;
    pHandler->onInitConnected();
    m_pHeaders = NULL;
    delete hdrs;

    if (isWantRead())
        pHandler->onReadEx();
    return LS_OK;
}


int QuicStream::shutdown()
{
    LS_DBG_L(this, "QuicStream::shutdown()");
    if (getState() >= HIOS_SHUTDOWN)
        return 0;
    setState(HIOS_SHUTDOWN);
    if (!m_pStream)
        return 0;
    setActiveTime(DateTime::s_curTime);
    //lsquic_stream_wantwrite(m_pStream, 1);
    //return lsquic_stream_shutdown(m_pStream, 1);
    return lsquic_stream_close(m_pStream);
}




int QuicStream::onTimer()
{
    if (getState() == HIOS_CONNECTED && getHandler())
        return getHandler()->onTimerEx();
    return 0;
}


void QuicStream::switchWriteToRead()
{
    return;
    //lsquic_stream_wantwrite(0);
    //lsquic_stream_wantread(1);
}


void QuicStream::continueWrite()
{
    LS_DBG_L(this, "QuicStream::continueWrite()");
    if (!m_pStream)
        return;
    setFlag(HIO_FLAG_WANT_WRITE, 1);
    lsquic_stream_wantwrite(m_pStream, 1);
}


void QuicStream::suspendWrite()
{
    LS_DBG_L(this, "QuicStream::suspendWrite()");
    if (!m_pStream)
        return;
    setFlag(HIO_FLAG_WANT_WRITE, 0);
    lsquic_stream_wantwrite(m_pStream, 0);
}


void QuicStream::continueRead()
{
    LS_DBG_L(this, "QuicStream::continueRead()");
    if (!m_pStream)
        return;
    setFlag(HIO_FLAG_WANT_READ, 1);
    lsquic_stream_wantread(m_pStream, 1);
}


void QuicStream::suspendRead()
{
    LS_DBG_L(this, "QuicStream::suspendRead()");
    if (!m_pStream)
        return;
    setFlag(HIO_FLAG_WANT_READ, 0);
    lsquic_stream_wantread(m_pStream, 0);
}


int QuicStream::sendRespHeaders(HttpRespHeaders *pRespHeaders, int isNoBody)
{
    struct iovec *pCur, *pEnd;
    lsquic_http_headers_t headers;
    struct iovec  headerList[1024];
    headers.count = 0;
    headers.headers = (lsquic_http_header *)headerList;
    pCur = headerList;
    pEnd = &headerList[1024];

    if (!m_pStream)
        return -1;

    char *p = (char *)HttpStatusCode::getInstance().getCodeString(
                  pRespHeaders->getHttpCode());
    pCur->iov_base  = (char *)":status";
    pCur->iov_len   = 7;
    ++pCur;
    pCur->iov_base = p + 1;
    pCur->iov_len  = 3;
    ++pCur;

    pRespHeaders->dropConnectionHeaders();

    for (int pos = pRespHeaders->HeaderBeginPos();
         pos != pRespHeaders->HeaderEndPos();
         pos = pRespHeaders->nextHeaderPos(pos))
    {
        int idx;
        int count = pRespHeaders->getHeader(pos, &idx, pCur, pCur+1,
                                        (pEnd - pCur - 1) / 2);

        if (count <= 0)
            continue;

        char *p = (char *)pCur->iov_base;
        char *pKeyEnd = p + pCur->iov_len;
        //to lowercase
        while (p < pKeyEnd)
        {
            *p = tolower(*p);
            ++p;
        }

        for(int i = count - 1; i > 0; --i)
        {
            pCur[(i << 1) | 1] = pCur[i + 1];
            pCur[(i << 1) ]    = *pCur;
        }
        pCur += (count << 1);

    }
    headers.count = (pCur - headerList) >> 1;
    return lsquic_stream_send_headers(m_pStream, &headers, isNoBody);
}


int QuicStream::sendfile(int fdSrc, off_t off, size_t size, int flag)
{
    return 0;
}


int QuicStream::checkReadRet(int ret)
{
    switch(ret)
    {
    case 0:
        if (getState() != HIOS_SHUTDOWN)
        {
            LS_DBG_L(this, "End of stream detected, CLOSING!");
#ifdef _ENTERPRISE_   //have the connection closed quickly
            setFlag(HIO_FLAG_PEER_SHUTDOWN, 1);
#endif
            setState(HIOS_CLOSING);
        }
        return -1;
    case -1:
        switch(errno)
        {
        case EAGAIN:
#if EAGAIN != EWOULDBLOCK
        case EWOULDBLOCK:
#endif
            return 0;
        default:
            tobeClosed();
            LS_DBG_L(this, "read error: %s\n", strerror(errno));
        }
    default:
        bytesRecv(ret);
        setActiveTime(DateTime::s_curTime);

    }
    return ret;

}


int QuicStream::readv(iovec *vector, int count)
{
    if (!m_pStream)
        return -1;
    ssize_t ret = lsquic_stream_readv(m_pStream, vector, count);
    LS_DBG_L(this, "QuicStream::readv(), ret: %zd", ret);
    return checkReadRet(ret);
}


int QuicStream::read(char *pBuf, int size)
{
    if (!m_pStream)
        return -1;
    ssize_t ret = lsquic_stream_read(m_pStream, pBuf, size);
    LS_DBG_L(this, "QuicStream::read(), to read: %d, ret: %zd", size, ret);
    return checkReadRet(ret);

}


int QuicStream::push(ls_str_t *pUrl, ls_str_t *pHost,
                   ls_strpair_t *pExtraHeaders)
{
    lsquic_conn_t *pConn = lsquic_stream_conn(m_pStream);
    lsquic_http_headers_t headers;
    ls_strpair_t *p = pExtraHeaders;
    int pushed;

    if (!m_pStream)
        return -1;

    while(p && p->key.ptr != NULL)
    {
        ++p;
    }
    headers.count = p - pExtraHeaders;
    headers.headers = (lsquic_http_header_t *)pExtraHeaders;

    pushed = lsquic_conn_push_stream(pConn, NULL, m_pStream,
                                   (const struct iovec*) pUrl,
                                   (const struct iovec*) pHost,
                                   &headers);
    if (pushed == 0)
        return 0;

    LS_DBG_L("push_stream() returned %d, unset flag", pushed);
    setFlag(HIO_FLAG_PUSH_CAPABLE, 0);
    return -1;
}


int QuicStream::close()
{
    LS_DBG_L(this, "QuicStream::close()");
    if (!m_pStream)
        return 0;
    return lsquic_stream_close(m_pStream);
}


int QuicStream::flush()
{
    LS_DBG_L(this, "QuicStream::flush()");
    if (!m_pStream)
        return -1;
    return lsquic_stream_flush(m_pStream);
}


int QuicStream::writev(const iovec *vector, int count)
{
    if (!m_pStream)
        return -1;
    ssize_t ret = lsquic_stream_writev(m_pStream, vector, count);
    LS_DBG_L(this, "QuicStream::writev(), ret: %zd, errno: %d", ret, errno);
    if (ret > 0)
        setActiveTime(DateTime::s_curTime);
    else if (ret == -1)
    {
        tobeClosed();
        LS_DBG_L(this, "close stream, writev error: %s\n", strerror(errno));
    }
    return ret;
}


int QuicStream::write(const char *pBuf, int size)
{
    if (!m_pStream)
        return -1;
    ssize_t ret = lsquic_stream_write(m_pStream, pBuf, size);
    LS_DBG_L(this, "QuicStream::writev(), to write: %d, ret: %zd, errno: %d",
             size, ret, errno);
    if (ret > 0)
        setActiveTime(DateTime::s_curTime);
    else if (ret == -1)
    {
        tobeClosed();
        LS_DBG_L(this, "close stream, write error: %s\n", strerror(errno));
    }

    return ret;
}


const char *QuicStream::buildLogId()
{
    const lsquic_cid_t *cid;
    lsquic_stream_id_t streamid;
    char cidstr[MAX_CID_LEN * 2 + 1];

    if (!m_pStream)
    {
        cidstr[0] = '\0';
        streamid = ~0ull;
    }
    else
    {
        lsquic_conn_t *pConn = lsquic_stream_conn(m_pStream);
        cid = lsquic_conn_id(pConn);
        lsquic_cid2str(cid, cidstr);
        streamid = lsquic_stream_id(m_pStream);
    }
    m_logId.len = ls_snprintf(m_logId.ptr, MAX_LOGID_LEN, "%s:%d-Q:%s-%" PRIu64,
                      getConnInfo()->m_pClientInfo->getAddrString(),
                      getConnInfo()->m_remotePort, cidstr, streamid);
    return m_logId.ptr;
}


void QuicStream::onRead()
{
    LS_DBG_L(this, "QuicStream::onRead()");
    if (getHandler())
    {
        getHandler()->onReadEx();
    }
    else if (getState() == HIOS_CONNECTED)
    {
        QuicUpkdHdrs *hdrs = (QuicUpkdHdrs *)lsquic_stream_get_hset(m_pStream);
        if (hdrs)
        {
            setActiveTime(DateTime::s_curTime);
            if (processUpkdHdrs(hdrs) == LS_FAIL)
            {
                LS_DBG_L(this, "QuicStream::processUpkdHdrs() failed, shutdown.");
                shutdown();
                return;
            }
        }
        else
        {
            LS_DBG_L(this, "lsquic_stream_get_hset() failed, shutdown.");
            shutdown();
            return;
        }
    }

    if (getState() == HIOS_CLOSING)
        onPeerClose();
}


void QuicStream::onWrite()
{
    LS_DBG_L(this, "QuicStream::onWrite()");
    if (getState() != HIOS_CONNECTED)
        close();
    else
    {
        if (getHandler())
            getHandler()->onWriteEx();
        if (getState() == HIOS_CLOSING)
            onPeerClose();
    }
}


void QuicStream::onClose()
{
    LS_DBG_L(this, "QuicStream::onClose()");
    if (getHandler())
        getHandler()->onCloseEx();
    m_pStream = NULL;
}


int QuicStream::getEnv(HioCrypto::ENV id, char *&val, int maxValLen)
{
    if (!m_pStream)
        return 0;
    lsquic_conn_t *pConn = lsquic_stream_conn(m_pStream);
    const char *str;
    int len, size;
    enum lsquic_version ver;

    if (maxValLen < 0)
        return 0;

    /* In the code below, we always NUL-terminate the string */

    switch(id)
    {
    case CRYPTO_VERSION:
        switch (lsquic_conn_crypto_ver(pConn))
        {
        case LSQ_CRY_QUIC:
            str = "QUIC";
            len = 4;
            break;
        default:
            str = "TLSv13";
            len = 6;
            break;
        }
        if (len + 1 > maxValLen)
            return 0;
        memcpy(val, str, len + 1);
        return len;

    case SESSION_ID:
        return 0;
//         if (sizeof(cid) * 2 + 1 > (unsigned) maxValLen)
//             return -1;
//         cid = lsquic_conn_id(pConn);
//         snprintf(val, maxValLen - 1, "%016"PRIx64, cid);
//         return sizeof(cid) * 2;

    case CLIENT_CERT:
        return 0;

    case CIPHER:
        str = lsquic_conn_crypto_cipher(pConn);
        if (!str)
            return 0;
        len = strlen(str);
        if (len + 1 > maxValLen)
            return 0;
        memcpy(val, str, len + 1);
        return len;

    case CIPHER_USEKEYSIZE:
        size = lsquic_conn_crypto_keysize(pConn);
        if (size < 0)
            return 0;
        len = snprintf(val, maxValLen - 1, "%d", size << 3);
        if (len > maxValLen - 1)
            return 0;
        return len;

    case CIPHER_ALGKEYSIZE:
        size = lsquic_conn_crypto_alg_keysize(pConn);
        if (size < 0)
            return 0;
        len = snprintf(val, maxValLen - 1, "%d", size << 3);
        if (len > maxValLen - 1)
            return 0;
        return len;

    case TRANS_PROTOCOL_VERSION:
        ver = lsquic_conn_quic_version(pConn);
        if (ver < N_LSQVER)
        {
            str = lsquic_ver2str[ver];
            len = strlen(str);
            if (len + 1 <= maxValLen)
            {
                memcpy(val, str, len + 1);
                return len;
            }
        }
        return 0;

    default:
        return 0;
    }
}

