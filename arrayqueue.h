#ifndef ARRAYQUEUE_H
#define ARRAYQUEUE_H

#include <QMutex>
#include <QDebug>
#include <memory>
#include <cstdint>

/**
 * @brief 图像数据节点结构体
 *
 * 用于存储单帧图像的完整信息
 */
struct ImageNode
{
    std::unique_ptr<unsigned char[]> pData;  // 图像数据缓冲区(智能指针管理)
    uint64_t nFrameLen;                       // 图像数据长度(字节)
    unsigned int nWidth;                      // 图像宽度(像素)
    unsigned int nHeight;                     // 图像高度(像素)
    unsigned int nFrameNum;                   // 帧序号

    // 构造函数：初始化指定大小的缓冲区
    explicit ImageNode(uint64_t bufferSize = 0)
        : nFrameLen(0)
        , nWidth(0)
        , nHeight(0)
        , nFrameNum(0)
    {
        if (bufferSize > 0)
        {
            pData = std::make_unique<unsigned char[]>(bufferSize);
        }
    }

    // 禁用拷贝构造和拷贝赋值(避免深拷贝开销)
    ImageNode(const ImageNode&) = delete;
    ImageNode& operator=(const ImageNode&) = delete;

    // 启用移动构造和移动赋值
    ImageNode(ImageNode&&) noexcept = default;
    ImageNode& operator=(ImageNode&&) noexcept = default;
};

/**
 * @brief 线程安全的循环队列
 *
 * 用于在图像采集线程和处理线程之间传递图像数据
 * 采用固定大小的循环队列实现，支持生产者-消费者模式
 */
class ArrayQueue
{
public:
    /**
     * @brief 构造函数
     */
    ArrayQueue();

    /**
     * @brief 析构函数
     * 自动释放所有资源
     */
    ~ArrayQueue();

    /**
     * @brief 初始化队列
     * @param nBufCount 队列容量(缓冲区个数)
     * @param defaultImageLen 单帧图像默认大小(字节)
     * @return 0-成功, 非0-失败(错误码)
     */
    int init(int nBufCount, uint64_t defaultImageLen);

    /**
     * @brief 将图像数据推入队列(生产者)
     * @param nFrameNum 帧序号
     * @param nWidth 图像宽度
     * @param nHeight 图像高度
     * @param pData 图像数据指针
     * @param nFrameLen 图像数据长度
     * @return 0-成功, MV_E_BUFOVER-队列已满
     */
    int push(int nFrameNum, int nWidth, int nHeight, const unsigned char* pData, uint64_t nFrameLen);

    /**
     * @brief 从队列中取出图像数据(消费者)
     * @param nFrameNum 输出-帧序号
     * @param nHeight 输出-图像高度
     * @param nWidth 输出-图像宽度
     * @param pData 输出缓冲区指针
     * @param nFrameLen 输出-图像数据长度
     * @return 0-成功, MV_E_NODATA-队列为空
     */
    int poll(int& nFrameNum, int& nHeight, int& nWidth, unsigned char* pData, uint64_t& nFrameLen);

    /**
     * @brief 获取队列当前元素数量
     * @return 当前队列中的元素个数
     */
    int size() const;

    /**
     * @brief 判断队列是否为空
     * @return true-空, false-非空
     */
    bool isEmpty() const;

    /**
     * @brief 判断队列是否已满
     * @return true-满, false-未满
     */
    bool isFull() const;

    // 错误码定义
    static const int OK = 0;
    static const int E_RESOURCE = 0x80000006;  // 资源申请失败
    static const int E_BUFOVER = 0x80000002;   // 缓存已满
    static const int E_NODATA = 0x80000007;    // 无数据

private:
    std::unique_ptr<ImageNode[]> m_queue;  // 队列数组(智能指针管理)
    int m_size;                             // 当前元素数量
    int m_start;                            // 队列头索引
    int m_end;                              // 队列尾索引
    int m_capacity;                         // 队列容量
    mutable QMutex m_mutex;                 // 互斥锁(成员变量，非全局)

    // 禁用拷贝和赋值
    ArrayQueue(const ArrayQueue&) = delete;
    ArrayQueue& operator=(const ArrayQueue&) = delete;
};

#endif // ARRAYQUEUE_H
