#include "HikLightController.h"
#include <QDebug>
#include <QElapsedTimer>

/**
 * @brief 构造函数
 * 初始化串口对象和状态变量
 */
HikLightController::HikLightController()
    : m_serialPort(nullptr)
    , m_currentBrightness(0)
    , m_isLightOn(false)
{
    m_serialPort = new QSerialPort();
}

/**
 * @brief 析构函数
 * 关闭串口并释放资源
 */
HikLightController::~HikLightController()
{
    closeSerial();
    if (m_serialPort)
    {
        delete m_serialPort;
        m_serialPort = nullptr;
    }
}

// ==================== 串口控制 ====================

/**
 * @brief 初始化串口
 * 配置串口参数并打开串口连接
 */
bool HikLightController::initSerial(const QString &portName)
{
    if (m_serialPort->isOpen())
    {
        qDebug() << "串口已打开";
        return false;
    }

    // 配置串口参数（严格按照光源控制器文档要求）
    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(QSerialPort::Baud19200);      // 波特率19200
    m_serialPort->setDataBits(QSerialPort::Data8);          // 数据位8
    m_serialPort->setStopBits(QSerialPort::OneStop);        // 停止位1
    m_serialPort->setParity(QSerialPort::NoParity);         // 无校验
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl); // 无流控

    // 尝试打开串口
    if (m_serialPort->open(QIODevice::ReadWrite))
    {
        qDebug() << "串口" << portName << "打开成功";
        return true;
    }

    qDebug() << "串口打开失败:" << m_serialPort->errorString();
    return false;
}

void HikLightController::closeSerial()
{
    if (m_serialPort && m_serialPort->isOpen())
    {
        m_serialPort->close();
        qDebug() << "串口已关闭";
    }
}

// ==================== 内部工具函数 ====================

/**
 * @brief 发送指令到串口
 * @param command 指令字符串
 * @return true-发送成功, false-发送失败
 */
bool HikLightController::sendCommand(const QString &command)
{
    if (!m_serialPort->isOpen())
    {
        qDebug() << "串口未打开";
        return false;
    }

    // 丢弃超时后才到达的陈旧回包，避免被本次命令误读
    m_serialPort->clear(QSerialPort::Input);

    // 转换为字节数组并发送
    QByteArray cmdData = command.toUtf8();
    qint64 writeLen = m_serialPort->write(cmdData);
    m_serialPort->flush();

    if (writeLen == cmdData.length())
    {
        qDebug() << "发送指令:" << command;
        return true;
    }

    qDebug() << "指令发送失败";
    return false;
}

/**
 * @brief 等待串口响应数据
 *
 * 同步阻塞读取（不处理事件循环，避免UI槽函数重入）。
 * 协议回复：查询亮度为"aXXXX"(5字节)，其余为单字符("A"/"H"/"L")，
 * 回复可能分多块到达，读取直到回复完整或超时。
 * @param timeoutMs 超时时间(毫秒)，默认1000ms
 * @return 响应字符串，超时或失败返回已收到的部分(可能为空)
 */
QString HikLightController::waitForResponse(int timeoutMs)
{
    if (!m_serialPort->isOpen())
    {
        return QString();
    }

    QByteArray responseData;
    QElapsedTimer timer;
    timer.start();

    while (true)
    {
        // 回复完整性判断：以'a'开头的查询回复为5字节，其余为1字节
        if (!responseData.isEmpty())
        {
            const int expectedLen = (responseData.at(0) == 'a') ? 5 : 1;
            if (responseData.size() >= expectedLen)
            {
                break;
            }
        }

        const qint64 remaining = timeoutMs - timer.elapsed();
        if (remaining <= 0 || !m_serialPort->waitForReadyRead(static_cast<int>(remaining)))
        {
            break;
        }
        responseData.append(m_serialPort->readAll());
    }

    QString response = QString::fromUtf8(responseData).trimmed();
    if (!response.isEmpty())
    {
        qDebug() << "收到响应:" << response;
    }

    return response;
}

// ==================== 光源控制 ====================

/**
 * @brief 设置光源亮度
 * 发送亮度设置指令到光源控制器
 * 指令格式：SAXXXX#，其中XXXX为4位十进制亮度值(如SA0168#表示设置亮度为168)
 */
bool HikLightController::setLightBrightness(int brightness)
{
    // 验证亮度范围
    if (brightness < 0 || brightness > 255)
    {
        qDebug() << "亮度值超出范围(0-255)";
        return false;
    }

    // 格式化为4位十进制字符串（如5→"0005"，168→"0168"）
    QString brightnessStr = QString("%1").arg(brightness, 4, 10, QChar('0'));
    QString cmd = QString("SA%1#").arg(brightnessStr);

    // 发送指令并等待响应
    if (sendCommand(cmd))
    {
        QString response = waitForResponse(1000);
        if (response == "A")
        {
            m_currentBrightness = brightness;
            return true;
        }
    }

    return false;
}

/**
 * @brief 打开光源
 * 发送常亮指令"SH#"到光源控制器
 */
bool HikLightController::setLightOn()
{
    if (sendCommand("SH#"))
    {
        QString response = waitForResponse(1000);
        if (response == "H")
        {
            m_isLightOn = true;
            return true;
        }
    }

    return false;
}

/**
 * @brief 关闭光源
 * 发送常灭指令"SL#"到光源控制器
 */
bool HikLightController::setLightOff()
{
    if (sendCommand("SL#"))
    {
        QString response = waitForResponse(1000);
        if (response == "L")
        {
            m_isLightOn = false;
            return true;
        }
    }

    return false;
}

// ==================== 状态查询 ====================

/**
 * @brief 查询光源亮度
 * 发送查询指令"SA#"到光源控制器，从设备读取当前亮度值
 * @return 查询到的亮度值
 */
int HikLightController::queryLightBrightness()
{
    if (sendCommand("SA#"))
    {
        // 亮度查询响应：格式为"aXXXX"，XXXX为4位十进制亮度值
        QString response = waitForResponse(1000);
        if (response.startsWith("a") && response.length() >= 5)
        {
            bool ok;
            int brightness = response.mid(1, 4).toInt(&ok);
            if (ok && brightness >= 0 && brightness <= 255)
            {
                m_currentBrightness = brightness;
            }
        }
    }

    return m_currentBrightness;
}

bool HikLightController::isLightOn() const
{
    return m_isLightOn;
}
