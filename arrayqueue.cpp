#include "arrayqueue.h"
#include <QDebug>
#include <cstring>
#include <new>
#include <utility>

int ArrayQueue::init(int nBufCount, uint64_t bufferSize)
{
    QMutexLocker locker(&m_mutex);

    if (nBufCount <= 0 || bufferSize == 0)
    {
        qWarning() << "ArrayQueue::init() - Invalid parameters: nBufCount=" << nBufCount
                   << ", bufferSize=" << bufferSize;
        return E_RESOURCE;
    }

    try
    {
        m_queue = std::make_unique<ImageNode[]>(nBufCount);
        for (int i = 0; i < nBufCount; ++i)
        {
            m_queue[i].pData = std::make_unique<unsigned char[]>(bufferSize);
        }
    }
    catch (const std::bad_alloc& e)
    {
        qCritical() << "ArrayQueue::init() - Memory allocation failed:" << e.what();
        m_queue.reset();
        m_capacity = 0;
        m_bufferSize = 0;
        return E_RESOURCE;
    }

    m_capacity = nBufCount;
    m_bufferSize = bufferSize;
    m_size = m_start = m_end = 0;
    return OK;
}

int ArrayQueue::push(const MV_FRAME_OUT_INFO_EX& info, const unsigned char* pData)
{
    QMutexLocker locker(&m_mutex);

    if (!pData || info.nFrameLenEx == 0 || info.nFrameLenEx > m_bufferSize)
    {
        qWarning() << "ArrayQueue::push() - Invalid frame: length" << info.nFrameLenEx
                   << ", buffer size" << m_bufferSize;
        return E_RESOURCE;
    }

    if (m_size >= m_capacity)
    {
        return E_BUFOVER;
    }

    ImageNode& node = m_queue[m_end];
    std::memcpy(node.pData.get(), pData, info.nFrameLenEx);
    node.nFrameLen = info.nFrameLenEx;
    node.nWidth = info.nExtendWidth;
    node.nHeight = info.nExtendHeight;
    node.nFrameNum = info.nFrameNum;
    node.enPixelType = info.enPixelType;

    ++m_size;
    m_end = (m_end + 1) % m_capacity;
    m_notEmpty.wakeOne();
    return OK;
}

int ArrayQueue::poll(ImageNode& out, unsigned long timeoutMs)
{
    QMutexLocker locker(&m_mutex);

    if (m_size == 0 && !m_notEmpty.wait(&m_mutex, timeoutMs))
    {
        return E_NODATA;
    }
    if (m_size == 0)
    {
        return E_NODATA;
    }

    std::swap(out, m_queue[m_start]);
    --m_size;
    m_start = (m_start + 1) % m_capacity;
    return OK;
}
