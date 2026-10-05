#include "PLCManager.h"
#include <QDebug>

// ==================== 构造和析构函数 ====================

/**
 * @brief 构造函数
 * 初始化PLC管理器
 */
PLCManager::PLCManager(QObject *parent)
    : QObject(parent)
    , m_client(nullptr)
    , m_connected(false)
{
}

/**
 * @brief 析构函数
 * 自动断开PLC连接并释放资源
 */
PLCManager::~PLCManager()
{
    // 在析构时，直接清理资源，不发送信号
    if (m_client)
    {
        m_client->Disconnect();
        delete m_client;
        m_client = nullptr;
    }
    m_connected = false;
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
    // 如果已经连接，先断开
    if (m_connected)
    {
        disconnectFromPLC();
    }

    // 创建Snap7客户端实例
    m_client = new TS7Client();
    if (!m_client)
    {
        m_lastError = "Failed to create Snap7 client";
        qDebug() << m_lastError;
        return false;
    }

    // 连接到PLC
    int result = m_client->ConnectTo(ip.toLocal8Bit().constData(), rack, slot);
    if (result == 0)
    {
        m_connected = true;
        m_lastError.clear();
        qDebug() << "PLC connected successfully:" << ip;
        emit connectionChanged(true);
        return true;
    }
    else
    {
        setError(result);
        qDebug() << "PLC connection failed:" << m_lastError;
        delete m_client;
        m_client = nullptr;
        return false;
    }
}

/**
 * @brief 断开PLC连接
 * @return 成功返回true，失败返回false
 */
bool PLCManager::disconnectFromPLC()
{
    if (!m_client)
    {
        m_connected = false;
        emit connectionChanged(false);
        return true;
    }

    int result = m_client->Disconnect();
    if (result == 0)
    {
        qDebug() << "PLC disconnected successfully";
        m_lastError.clear();
    }
    else
    {
        setError(result);
        qDebug() << "PLC disconnection warning:" << m_lastError;
    }

    delete m_client;
    m_client = nullptr;
    m_connected = false;
    emit connectionChanged(false);

    return true;
}

/**
 * @brief 检查是否已连接到PLC
 * @return 已连接返回true，否则返回false
 */
bool PLCManager::isConnected() const
{
    return m_connected && m_client != nullptr;
}

// ==================== 数据读写函数 ====================

/**
 * @brief 写入PLC V区单个位
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

    // 先读取当前字节的值
    byte buffer = 0;
    int result = m_client->DBRead(1, byteOffset, 1, &buffer);

    if (result != 0)
    {
        setError(result);
        return false;
    }

    // 修改指定位
    if (value)
    {
        buffer |= (1 << bitPos);  // 置1
    }
    else
    {
        buffer &= ~(1 << bitPos); // 清0
    }

    // 写回PLC的V区
    result = m_client->DBWrite(1, byteOffset, 1, &buffer);

    if (result == 0)
    {
        m_lastError.clear();
        return true;
    }
    else
    {
        setError(result);
        return false;
    }
}

/**
 * @brief 读取PLC V区单个位
 * @param byteOffset 字节偏移
 * @param bitPos 位位置（0-7）
 * @return 成功返回true，失败返回false
 */
bool PLCManager::readVBit(int byteOffset, int bitPos)
{
    if (!isConnected())
    {
        m_lastError = "PLC not connected";
        return false;
    }

    // 读取当前字节的值
    byte buffer = 0;
    int result = m_client->DBRead(1, byteOffset, 1, &buffer);

    if (result != 0)
    {
        setError(result);
        return false;
    }

    m_lastError.clear();
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
