#include "arrayqueue.h"
#include <QMutexLocker>
#include <algorithm>

/**
 * @brief 构造函数
 * 初始化队列为空状态
 */
ArrayQueue::ArrayQueue()
    : m_queue(nullptr)
    , m_size(0)
    , m_start(0)
    , m_end(0)
    , m_capacity(0)
{
    // 成员初始化列表已完成所有初始化工作
}

/**
 * @brief 析构函数
 * 由于使用智能指针，资源会自动释放，无需手动管理
 */
ArrayQueue::~ArrayQueue()
{
    // QMutex 和 unique_ptr 会自动析构
    // 不需要手动加锁或释放内存
}

/**
 * @brief 初始化队列
 * @param nBufCount 队列容量
 * @param defaultImageLen 单帧图像默认大小
 * @return 0-成功, MV_E_RESOURCE-资源分配失败
 */
int ArrayQueue::init(int nBufCount, uint64_t defaultImageLen)
{
    QMutexLocker locker(&m_mutex);  // RAII 自动加锁/解锁

    // 参数校验
    if (nBufCount <= 0 || defaultImageLen == 0)
    {
        qWarning() << "ArrayQueue::init() - Invalid parameters: nBufCount="
                   << nBufCount << ", defaultImageLen=" << defaultImageLen;
        return E_RESOURCE;
    }

    try
    {
        // 创建队列数组
        m_queue = std::make_unique<ImageNode[]>(nBufCount);
        m_capacity = nBufCount;

        // 为每个节点分配缓冲区
        for (int i = 0; i < nBufCount; ++i)
        {
            m_queue[i].pData = std::make_unique<unsigned char[]>(defaultImageLen);
            if (!m_queue[i].pData)
            {
                qCritical() << "ArrayQueue::init() - Failed to allocate buffer for node" << i;
                return E_RESOURCE;
            }

            // 初始化节点信息
            m_queue[i].nFrameNum = 0;
            m_queue[i].nHeight = 0;
            m_queue[i].nWidth = 0;
            m_queue[i].nFrameLen = 0;
        }

        qDebug() << "ArrayQueue initialized successfully: capacity=" << nBufCount
                 << ", bufferSize=" << defaultImageLen;
        return OK;
    }
    catch (const std::bad_alloc& e)
    {
        qCritical() << "ArrayQueue::init() - Memory allocation failed:" << e.what();
        m_queue.reset();  // 释放已分配的内存
        m_capacity = 0;
        return E_RESOURCE;
    }
}

/**
 * @brief 将图像数据推入队列
 * @param nFrameNum 帧序号
 * @param nWidth 图像宽度
 * @param nHeight 图像高度
 * @param pData 图像数据指针
 * @param nFrameLen 图像数据长度
 * @return 0-成功, MV_E_BUFOVER-队列已满
 */
int ArrayQueue::push(int nFrameNum, int nWidth, int nHeight, const unsigned char* pData, uint64_t nFrameLen)
{
    QMutexLocker locker(&m_mutex);  // RAII 自动加锁/解锁

    // 检查队列是否已满
    if (m_size >= m_capacity)
    {
        qWarning() << "ArrayQueue::push() - Queue is full, dropping frame" << nFrameNum;
        return E_BUFOVER;
    }

    // 参数校验
    if (!pData || nFrameLen == 0)
    {
        qWarning() << "ArrayQueue::push() - Invalid data pointer or length";
        return E_RESOURCE;
    }

    // 存储图像信息
    m_queue[m_end].nFrameNum = static_cast<unsigned int>(nFrameNum);
    m_queue[m_end].nHeight = static_cast<unsigned int>(nHeight);
    m_queue[m_end].nWidth = static_cast<unsigned int>(nWidth);
    m_queue[m_end].nFrameLen = nFrameLen;

    // 拷贝图像数据
    if (m_queue[m_end].pData)
    {
        std::copy(pData, pData + nFrameLen, m_queue[m_end].pData.get());
    }
    else
    {
        qCritical() << "ArrayQueue::push() - Buffer not allocated for index" << m_end;
        return E_RESOURCE;
    }

    // 更新队列状态
    ++m_size;
    m_end = (m_end + 1) % m_capacity;  // 循环队列

    return OK;
}

/**
 * @brief 从队列中取出图像数据
 * @param nFrameNum 输出-帧序号
 * @param nHeight 输出-图像高度
 * @param nWidth 输出-图像宽度
 * @param pData 输出缓冲区指针
 * @param nFrameLen 输出-图像数据长度
 * @return 0-成功, MV_E_NODATA-队列为空
 */
int ArrayQueue::poll(int& nFrameNum, int& nHeight, int& nWidth, unsigned char* pData, uint64_t& nFrameLen)
{
    QMutexLocker locker(&m_mutex);  // RAII 自动加锁/解锁

    // 检查队列是否为空
    if (m_size == 0)
    {
        return E_NODATA;
    }

    // 参数校验
    if (!pData)
    {
        qWarning() << "ArrayQueue::poll() - Invalid output buffer pointer";
        return E_RESOURCE;
    }

    // 读取图像信息
    nFrameNum = static_cast<int>(m_queue[m_start].nFrameNum);
    nHeight = static_cast<int>(m_queue[m_start].nHeight);
    nWidth = static_cast<int>(m_queue[m_start].nWidth);
    nFrameLen = m_queue[m_start].nFrameLen;

    // 拷贝图像数据
    if (m_queue[m_start].pData)
    {
        std::copy(m_queue[m_start].pData.get(),
                  m_queue[m_start].pData.get() + nFrameLen,
                  pData);
    }
    else
    {
        qCritical() << "ArrayQueue::poll() - Buffer not allocated for index" << m_start;
        return E_RESOURCE;
    }

    // 更新队列状态
    --m_size;
    m_start = (m_start + 1) % m_capacity;  // 循环队列

    return OK;
}

/**
 * @brief 获取队列当前元素数量
 * @return 当前队列中的元素个数
 */
int ArrayQueue::size() const
{
    QMutexLocker locker(&m_mutex);
    return m_size;
}

/**
 * @brief 判断队列是否为空
 * @return true-空, false-非空
 */
bool ArrayQueue::isEmpty() const
{
    QMutexLocker locker(&m_mutex);
    return m_size == 0;
}

/**
 * @brief 判断队列是否已满
 * @return true-满, false-未满
 */
bool ArrayQueue::isFull() const
{
    QMutexLocker locker(&m_mutex);
    return m_size >= m_capacity;
}
