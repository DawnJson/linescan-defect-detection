#ifndef PLCMANAGER_H
#define PLCMANAGER_H

#include <memory>
#include <QString>
#include <snap7.h>

/**
 * @brief PLC通信管理类
 *
 * 该类封装了PLC的连接、断开、读写等操作
 * 使用Snap7库与西门子PLC（S7-200 SMART）进行通信
 * 任何读写失败都会将状态置为未连接，调用方可通过reconnect()重连
 */
class PLCManager
{
public:
    PLCManager();
    ~PLCManager();

    PLCManager(const PLCManager&) = delete;
    PLCManager& operator=(const PLCManager&) = delete;

    /**
     * @brief 连接PLC，并保存连接参数供reconnect()使用
     * @param ip PLC的IP地址
     * @param rack 机架号（通常为0）
     * @param slot 槽号（通常为1）
     * @return 成功返回true，失败返回false
     */
    bool connectToPLC(const QString& ip, int rack, int slot);

    /**
     * @brief 断开PLC连接
     */
    void disconnectFromPLC();

    /**
     * @brief 检查PLC是否已连接
     * @return 已连接返回true，否则返回false
     */
    bool isConnected() const;

    /**
     * @brief 使用已保存的参数重新连接PLC（先断开再连接）
     * @return 成功返回true；从未配置过连接参数或连接失败返回false
     */
    bool reconnect();

    /**
     * @brief 原子写入PLC V区（DB1）单个位
     * @param byteOffset 字节偏移
     * @param bitPos 位位置（0-7）
     * @param value 要写入的值
     * @return 成功返回true，失败返回false（失败时置为未连接）
     */
    bool writeVBit(int byteOffset, int bitPos, bool value);

    /**
     * @brief 读取V区（DB1）byteOffset处1字节，检测PLC是否可达
     * @param byteOffset 字节偏移
     * @return 可达返回true，失败返回false（失败时置为未连接）
     */
    bool ping(int byteOffset);

    /**
     * @brief 获取最后的错误信息
     * @return 错误信息字符串
     */
    QString getLastError() const;

private:
    /**
     * @brief 设置错误信息
     * @param errorCode Snap7错误代码
     */
    void setError(int errorCode);

    /**
     * @brief 读写失败处理：记录错误并标记为未连接
     * @param errorCode Snap7错误代码
     */
    void markFailed(int errorCode);

    std::unique_ptr<TS7Client> m_client;    // Snap7客户端
    bool m_connected;                       // 连接状态标志
    QString m_lastError;                    // 最后的错误信息

    QString m_ip;                           // 保存的连接参数
    int m_rack;
    int m_slot;
    bool m_configured;                      // 是否已保存连接参数
};

#endif // PLCMANAGER_H
