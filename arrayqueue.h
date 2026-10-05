#ifndef ARRAYQUEUE_H
#define ARRAYQUEUE_H

#include <QMutex>
#include <QWaitCondition>
#include <memory>
#include <cstdint>
#include "MvCameraControl.h"

/**
 * @brief 单帧图像：数据缓冲区 + 帧信息
 *
 * 缓冲区大小固定为队列的 bufferSize()，取帧时与队列节点交换缓冲区，不拷贝像素。
 */
struct ImageNode
{
    std::unique_ptr<unsigned char[]> pData;    // 图像数据缓冲区
    uint64_t nFrameLen = 0;                    // 有效数据长度（字节）
    unsigned int nWidth = 0;                   // 图像宽度（像素）
    unsigned int nHeight = 0;                  // 图像高度（像素）
    unsigned int nFrameNum = 0;                // 帧序号
    MvGvspPixelType enPixelType = PixelType_Gvsp_Undefined;  // 像素格式
};

/**
 * @brief 相机回调（生产者）与处理线程（消费者）之间的定长环形队列
 *
 * push() 把 SDK 缓冲区拷贝进空闲节点；poll() 把节点缓冲区与调用方缓冲区交换，
 * 锁内只做指针交换，不会阻塞相机回调。
 */
class ArrayQueue
{
public:
    ArrayQueue() = default;
    ArrayQueue(const ArrayQueue&) = delete;
    ArrayQueue& operator=(const ArrayQueue&) = delete;

    /**
     * @brief 分配 nBufCount 个节点，每个节点 bufferSize 字节
     * @return OK 或 E_RESOURCE
     */
    int init(int nBufCount, uint64_t bufferSize);

    /// 每个节点缓冲区的字节数，poll() 的调用方缓冲区必须是这个大小
    uint64_t bufferSize() const { return m_bufferSize; }

    /**
     * @brief 拷贝一帧入队（生产者，相机回调线程）
     * @return OK、E_BUFOVER（队列满，丢帧）或 E_RESOURCE（参数无效或帧超过缓冲区）
     */
    int push(const MV_FRAME_OUT_INFO_EX& info, const unsigned char* pData);

    /**
     * @brief 取出一帧（消费者），队列为空时最多等待 timeoutMs 毫秒
     * @param out 调用方节点，其 pData 必须是 bufferSize() 字节；成功时与队列节点交换
     * @return OK 或 E_NODATA（超时）
     */
    int poll(ImageNode& out, unsigned long timeoutMs);

    // 错误码定义
    static const int OK = 0;
    static const int E_RESOURCE = 0x80000006;  // 资源申请失败
    static const int E_BUFOVER = 0x80000002;   // 缓存已满
    static const int E_NODATA = 0x80000007;    // 无数据

private:
    std::unique_ptr<ImageNode[]> m_queue;  // 节点数组
    uint64_t m_bufferSize = 0;             // 每个节点缓冲区大小
    int m_capacity = 0;                    // 节点个数
    int m_size = 0;                        // 当前帧数
    int m_start = 0;                       // 队头索引
    int m_end = 0;                         // 队尾索引
    QMutex m_mutex;
    QWaitCondition m_notEmpty;
};

#endif // ARRAYQUEUE_H
