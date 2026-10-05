#include "HikLightController.h"
#include <QCoreApplication>

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

bool HikLightController::isSerialOpen() const
{
    return m_serialPort && m_serialPort->isOpen();
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
 * @param timeoutMs 超时时间(毫秒)，默认1000ms
 * @return 响应字符串，超时或失败返回空字符串
 */
QString HikLightController::waitForResponse(int timeoutMs)
{
    if (!m_serialPort->isOpen())
    {
        return QString();
    }

    QByteArray responseData;
    int elapsed = 0;
    const int waitStep = 10;

    // 循环等待数据，每次等待10ms
    while (elapsed < timeoutMs)
    {
        if (m_serialPort->waitForReadyRead(waitStep))
        {
            responseData.append(m_serialPort->readAll());

            // 收到数据后再等待一小段时间，确保接收完整
            QCoreApplication::processEvents();
            if (m_serialPort->bytesAvailable() > 0)
            {
                responseData.append(m_serialPort->readAll());
            }
            break;
        }
        elapsed += waitStep;
        QCoreApplication::processEvents();
    }

    // 转换为字符串并解析
    QString response = QString::fromUtf8(responseData).trimmed();
    if (!response.isEmpty())
    {
        qDebug() << "收到响应:" << response;
        parseResponse(response);
    }

    return response;
}

/**
 * @brief 解析串口响应数据
 * 根据响应内容更新内部状态缓存
 * @param response 响应字符串
 */
void HikLightController::parseResponse(const QString &response)
{
    if (response.isEmpty())
    {
        return;
    }

    // 解析各种响应格式（按照光源控制器协议文档）
    if (response == "A")
    {
        // 亮度设置成功响应
        qDebug() << "亮度设置成功";
    }
    else if (response.startsWith("a") && response.length() >= 5)
    {
        // 亮度查询响应：格式为"aXXXX"，XXXX为4位十进制亮度值
        QString brightnessStr = response.mid(1, 4);
        bool ok;
        int brightness = brightnessStr.toInt(&ok);
        if (ok && brightness >= 0 && brightness <= 255)
        {
            m_currentBrightness = brightness;
            qDebug() << "读取亮度:" << brightness;
        }
    }
    else if (response == "H")
    {
        // 光源常亮响应
        m_isLightOn = true;
        qDebug() << "光源已开启";
    }
    else if (response == "L")
    {
        // 光源常灭响应
        m_isLightOn = false;
        qDebug() << "光源已关闭";
    }
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

int HikLightController::getCurrentBrightness() const
{
    return m_currentBrightness;
}

/**
 * @brief 查询光源亮度
 * 发送查询指令"SA#"到光源控制器，从设备读取当前亮度值
 * @return 查询到的亮度值
 */
int HikLightController::queryLightBrightness()
{
    if (sendCommand("SA#"))
    {
        QString response = waitForResponse(1000);
        if (response.startsWith("a"))
        {
            // parseResponse函数已自动更新m_currentBrightness
            return m_currentBrightness;
        }
    }

    return m_currentBrightness;
}

bool HikLightController::isLightOn() const
{
    return m_isLightOn;
}
