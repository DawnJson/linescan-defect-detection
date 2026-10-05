#include "PLCManager.h"
#include <QDebug>

namespace
{
const int kDbV = 1;                 // S7-200 SMART的V区对应DB1
const int kTimeoutMs = 1000;        // Ping/发送/接收超时，避免图像线程被长时间阻塞
}

// ==================== 构造和析构函数 ====================

/**
 * @brief 构造函数
 * 初始化PLC管理器
 */
PLCManager::PLCManager()
    : m_connected(false)
    , m_rack(0)
    , m_slot(0)
    , m_configured(false)
{
}

/**
 * @brief 析构函数
 * 自动断开PLC连接并释放资源
 */
PLCManager::~PLCManager()
{
    disconnectFromPLC();
}

// ==================== 连接管理函数 ====================

/**
 * @brief 连接到PLC
 * @param ip PLC的IP地址
 * @param rack 机架号
 * @param slot 槽号
 * @return 成功返回true，失败返回false
 */
bool PLCManager::connectToPLC(const QString& ip, int rack, int slot)
{
    disconnectFromPLC();

    m_ip = ip;
    m_rack = rack;
    m_slot = slot;
    m_configured = true;

    m_client.reset(new TS7Client());

    // 连接前设置较短的超时
    int timeout = kTimeoutMs;
    m_client->SetParam(p_i32_PingTimeout, &timeout);
    m_client->SetParam(p_i32_SendTimeout, &timeout);
    m_client->SetParam(p_i32_RecvTimeout, &timeout);

    int result = m_client->ConnectTo(m_ip.toLocal8Bit().constData(), rack, slot);
    if (result == 0)
    {
        m_connected = true;
        m_lastError.clear();
        qDebug() << "PLC connected successfully:" << m_ip;
        return true;
    }

    setError(result);
    m_client.reset();
    return false;
}

/**
 * @brief 断开PLC连接
 */
void PLCManager::disconnectFromPLC()
{
    if (m_client)
    {
        m_client->Disconnect();
        m_client.reset();
    }
    m_connected = false;
}

/**
 * @brief 检查是否已连接到PLC
 * @return 已连接返回true，否则返回false
 */
bool PLCManager::isConnected() const
{
    return m_connected && m_client;
}

/**
 * @brief 使用已保存的参数重新连接
 * @return 成功返回true，未配置或失败返回false
 */
bool PLCManager::reconnect()
{
    if (!m_configured)
    {
        m_lastError = "PLC connection parameters not configured";
        return false;
    }

    // connectToPLC会覆盖成员，这里使用副本
    const QString ip = m_ip;
    const int rack = m_rack;
    const int slot = m_slot;
    return connectToPLC(ip, rack, slot);
}

// ==================== 数据读写函数 ====================

/**
 * @brief 原子写入PLC V区单个位
 * @param byteOffset 字节偏移
 * @param bitPos 位位置（0-7）
 * @param value 要写入的值
 * @return 成功返回true，失败返回false
 */
bool PLCManager::writeVBit(int byteOffset, int bitPos, bool value)
{
    if (!isConnected())
    {
        m_lastError = "PLC not connected";
        return false;
    }

    if (byteOffset < 0 || bitPos < 0 || bitPos > 7)
    {
        m_lastError = "Invalid PLC address";
        return false;
    }

    byte bitValue = value ? 1 : 0;
    int result = m_client->WriteArea(S7AreaDB, kDbV, byteOffset * 8 + bitPos, 1, S7WLBit, &bitValue);
    if (result != 0)
    {
        markFailed(result);
        return false;
    }
    return true;
}

/**
 * @brief 读取V区1字节，检测PLC是否可达
 * @param byteOffset 字节偏移
 * @return 可达返回true，失败返回false
 */
bool PLCManager::ping(int byteOffset)
{
    if (!isConnected())
    {
        m_lastError = "PLC not connected";
        return false;
    }

    byte data = 0;
    int result = m_client->DBRead(kDbV, byteOffset, 1, &data);
    if (result != 0)
    {
        markFailed(result);
        return false;
    }
    return true;
}

// ==================== 错误处理函数 ====================

/**
 * @brief 获取最后的错误信息
 * @return 错误信息字符串
 */
QString PLCManager::getLastError() const
{
    return m_lastError;
}

/**
 * @brief 设置错误信息
 * @param errorCode Snap7错误代码
 */
void PLCManager::setError(int errorCode)
{
    m_lastError = QString("Snap7 error code: 0x%1").arg(errorCode, 0, 16);
}

/**
 * @brief 读写失败处理：记录错误并标记为未连接
 * @param errorCode Snap7错误代码
 */
void PLCManager::markFailed(int errorCode)
{
    setError(errorCode);
    m_connected = false;
    qDebug() << "PLC I/O failed:" << m_lastError;
}
