#ifndef PLCMANAGER_H
#define PLCMANAGER_H

#include <QObject>
#include <QString>
#include <snap7.h>

/**
 * @brief PLC通信管理类
 *
 * 该类封装了PLC的连接、断开、读写等操作
 * 使用Snap7库与西门子PLC进行通信
 */
class PLCManager : public QObject
{
    Q_OBJECT

public:
    explicit PLCManager(QObject *parent = nullptr);
    ~PLCManager();

    /**
     * @brief 连接PLC
     * @param ip PLC的IP地址
     * @param rack 机架号（通常为0）
     * @param slot 槽号（通常为1）
     * @return 成功返回true，失败返回false
     */
    bool connectToPLC(const QString& ip, int rack, int slot);

    /**
     * @brief 断开PLC连接
     * @return 成功返回true，失败返回false
     */
    bool disconnectFromPLC();

    /**
     * @brief 检查PLC是否已连接
     * @return 已连接返回true，否则返回false
     */
    bool isConnected() const;

    /**
     * @brief 写入PLC V区单个位
     * @param byteOffset 字节偏移
     * @param bitPos 位位置（0-7）
     * @param value 要写入的值
     * @return 成功返回true，失败返回false
     */
    bool writeVBit(int byteOffset, int bitPos, bool value);

    /**
     * @brief 读取PLC V区单个位
     * @param byteOffset 字节偏移
     * @param bitPos 位位置（0-7）
     * @return 成功返回true，失败返回false
     */
    bool readVBit(int byteOffset, int bitPos);

    /**
     * @brief 获取最后的错误信息
     * @return 错误信息字符串
     */
    QString getLastError() const;

signals:
    /**
     * @brief 连接状态改变信号
     * @param connected true表示已连接，false表示已断开
     */
    void connectionChanged(bool connected);

private:
    TS7Client* m_client;           // Snap7客户端指针
    bool m_connected;              // 连接状态标志
    QString m_lastError;           // 最后的错误信息

    /**
     * @brief 设置错误信息
     * @param errorCode 错误代码
     */
    void setError(int errorCode);
};

#endif // PLCMANAGER_H
