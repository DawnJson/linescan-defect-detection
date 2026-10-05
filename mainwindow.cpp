#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QSerialPortInfo>
#include <QStyle>
#include "branding.h"

// 常量定义
namespace {
    constexpr unsigned int INFINITE_TIMEOUT = 0xFFFFFFFF;
    constexpr int MV_TRIGGER_SOURCE_ENCODER_MODULE_OUT = 6;
    constexpr int MAX_QUEUE_COUNT = 50;  // 队列缓冲区个数
}

// ==================== UI 控制辅助函数 ====================

/**
 * @brief 设置设备控制按钮使能状态
 * @param enable true-设备已打开, false-设备未打开
 */
void MainWindow::setDeviceControlsEnabled(bool enable)
{
    ui->DeviceOpen->setEnabled(!enable);
    ui->DeviceClose->setEnabled(enable);
    ui->StartGrab->setEnabled(enable);
    ui->SearchDevice->setEnabled(!enable);
    setStatusChip(ui->CamStatusLabel, enable ? "已打开" : "未打开", enable ? "ok" : "off");
}

void MainWindow::setStatusChip(QLabel* label, const QString& text, const char* state)
{
    label->setText(text);
    label->setProperty("state", state);
    label->style()->unpolish(label);
    label->style()->polish(label);
}

void MainWindow::updateModelStatus()
{
    const bool loaded = ui->DetectCheckBox->isEnabled();
    setStatusChip(ui->ModelStatusLabel, loaded ? "已加载" : "未加载", loaded ? "ok" : "error");
}

/**
 * @brief 设置参数控制控件使能状态
 * @param enable true-启用, false-禁用
 */
void MainWindow::setParameterControlsEnabled(bool enable)
{
    ui->ExposureEdit->setEnabled(enable);
    ui->WidthEdit->setEnabled(enable);
    ui->HeightEdit->setEnabled(enable);
    ui->PreampGainBox->setEnabled(enable);
    ui->DigitalGainEdit->setEnabled(enable);
    // AcquisitionLineRateEdit的使能状态由AcquisitionLineRateEnableBox控制
    ui->AcquisitionLineRateEdit->setEnabled(enable && ui->AcquisitionLineRateEnableBox->isChecked());
    ui->AcquisitionLineRateEnableBox->setEnabled(enable);
    ui->ResultLineRateEdit->setEnabled(enable);
    ui->ResultFrameRateEdit->setEnabled(enable);
    ui->GetPara->setEnabled(enable);
    ui->SetPara->setEnabled(enable);
    ui->imgSavePathEdit->setEnabled(enable);
    ui->AcquisitionBurstFrameCountEdit->setEnabled(enable);
    ui->SavePara->setEnabled(enable);
    ui->LoadPara->setEnabled(enable);
}

/**
 * @brief 设置采集控制控件使能状态
 * @param enable true-启用, false-禁用
 */
void MainWindow::setAcquisitionControlsEnabled(bool enable)
{
    ui->TriggerSelectBox->setEnabled(enable);
    ui->TriggerModeBox->setEnabled(enable);
    ui->TriggerSourceBox->setEnabled(enable);
    ui->PixelFormatBox->setEnabled(enable);
    ui->HBFormatBox->setEnabled(enable);
}

/**
 * @brief 设置采集过程中的控件使能状态
 * @param isGrabbing true-正在采集, false-未采集
 */
void MainWindow::setGrabbingControlsEnabled(bool isGrabbing)
{
    ui->StartGrab->setEnabled(!isGrabbing);
    ui->StopGrab->setEnabled(isGrabbing);

    // 采集时禁用这些控件
    setParameterControlsEnabled(!isGrabbing);
    setAcquisitionControlsEnabled(!isGrabbing);
    ui->PixelFormatBox->setEnabled(!isGrabbing);
    ui->HBFormatBox->setEnabled(!isGrabbing);
    setStatusChip(ui->CamStatusLabel, isGrabbing ? "采集中" : "已打开", isGrabbing ? "busy" : "ok");
}

// ==================== 主程序代码 ====================

/**
 * @brief 构造函数
 * 初始化主窗口和所有成员变量
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , initialflag(false)
    , m_OpenDevice(false)
    , m_MyCamera(nullptr)
    , m_DeviceCombo(0)
    , m_ThreadState(false)
    , m_ProcessThread(nullptr)
    , m_StartGrabbing(false)
    , m_TriggerModeCheck(false)
    , m_bPreampGain(false)
    , m_bAcquisitionLineRate(false)
    , m_HBMode(false)
    , m_queue(nullptr)
    , m_nImageSize(0)
    , m_yoloDetector(nullptr)
    , m_enableDefectDetection(true)  // 默认启用检测
    , m_modelPath("")
    , m_lightController(nullptr)
{
    ui->setupUi(this);

    // 窗口标题来自 :/branding/branding.ini
    setWindowTitle(Branding::instance().windowTitle);

    // 初始化UI控件状态（设备未打开）
    setDeviceControlsEnabled(false);
    setParameterControlsEnabled(false);
    setAcquisitionControlsEnabled(false);
    ui->StopGrab->setEnabled(false);
    ui->SingleSoftTrigger->setEnabled(false);

    // 初始化 YOLO 检测器
    m_modelPath = ui->ModelPathEdit->text();

    // 检查模型文件是否存在
    QFileInfo modelFile(m_modelPath);
    if (modelFile.exists())
    {
        try
        {
            // 初始化推理选项
            trtyolo::InferOption option;
            option.enableSwapRB();  // 启用 RGB 通道交换（从 RGB 到 BGR）

            // 创建检测模型
            m_yoloDetector = std::make_unique<trtyolo::DetectModel>(m_modelPath.toStdString(), option);

            qDebug() << "YOLO 检测器初始化成功，模型路径:" << m_modelPath;

            // 设置 DetectCheckBox 可用且默认选中（检测功能默认启用）
            ui->DetectCheckBox->setEnabled(true);
            ui->DetectCheckBox->setChecked(true);
        }
        catch (const std::exception& e)
        {
            qCritical() << "YOLO 检测器初始化失败:" << e.what();
            m_enableDefectDetection = false;

            // 禁用 DetectCheckBox
            ui->DetectCheckBox->setEnabled(false);
            ui->DetectCheckBox->setChecked(false);
        }
    }
    else
    {
        qWarning() << "YOLO 模型文件不存在:" << m_modelPath;
        m_enableDefectDetection = false;

        // 禁用 DetectCheckBox
        ui->DetectCheckBox->setEnabled(false);
        ui->DetectCheckBox->setChecked(false);
    }
    updateModelStatus();

    // 缺陷数随列表变化（每行一个缺陷，每块板开始时清空）
    auto updateDefectCount = [this]() {
        ui->DefectCountLabel->setText(QString::number(ui->DefectListWidget->count()));
    };
    connect(ui->DefectListWidget->model(), &QAbstractItemModel::rowsInserted, this, updateDefectCount);
    connect(ui->DefectListWidget->model(), &QAbstractItemModel::rowsRemoved, this, updateDefectCount);
    connect(ui->DefectListWidget->model(), &QAbstractItemModel::modelReset, this, updateDefectCount);

    // ==================== 初始化光源控制器 ====================

    // 创建光源控制器对象
    m_lightController = new HikLightController();

    // 初始化串口列表并设置控件状态
    initLightPortList();
    updateLightControlsEnabled(false);

    // 设置亮度输入框的验证器（0-255）
    ui->LightnessEdit->setValidator(new QIntValidator(0, 255, this));

    // 初始化光源状态标签
    setStatusChip(ui->LightStatusLabel, "未连接", "off");

    // ==================== 初始化传送带控制器 ====================

    // 创建传送带控制器对象
    m_conveyorController = new ConveyorController(this);

    // 连接传送带控制器的信号
    connect(m_conveyorController, &ConveyorController::statusUpdated,
            this, &MainWindow::updateConveyorStatus);

    // 初始化串口列表并设置控件状态
    initConveyorPortList();
    updateConveyorControlsEnabled(false);

    // 初始化传送带状态标签
    setStatusChip(ui->ConSatus, "未连接", "off");

    // 自动搜索设备
    on_SearchDevice_clicked();
}

MainWindow::~MainWindow()
{
    // 关闭相机设备
    on_DeviceClose_clicked();

    // 清理光源控制器
    if (m_lightController)
    {
        m_lightController->closeSerial();
        delete m_lightController;
        m_lightController = nullptr;
    }

    // 清理传送带控制器
    if (m_conveyorController)
    {
        m_conveyorController->closeSerial();
        delete m_conveyorController;
        m_conveyorController = nullptr;
    }

    // 删除UI
    delete ui;
}

int MainWindow::GetTriggerSelector()
{
    MVCC_ENUMVALUE stEnumTriggerSelectorValue = { 0 };
    MVCC_ENUMENTRY stEnumTriggerSelectorEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("TriggerSelector", &stEnumTriggerSelectorValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->TriggerSelectBox->clear();
    for (int i = 0; i < stEnumTriggerSelectorValue.nSupportedNum; i++)
    {
        memset(&stEnumTriggerSelectorEntry, 0, sizeof(stEnumTriggerSelectorEntry));
        stEnumTriggerSelectorEntry.nValue = stEnumTriggerSelectorValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("TriggerSelector", &stEnumTriggerSelectorEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumTriggerSelectorEntry.chSymbolic);

        ui->TriggerSelectBox->addItem(qstrSymbolic);

    }

    // 设置当前值
    for (int i = 0; i < stEnumTriggerSelectorValue.nSupportedNum; i++)
    {
        if (stEnumTriggerSelectorValue.nCurValue == stEnumTriggerSelectorValue.nSupportValue[i])
        {
            ui->TriggerSelectBox->setCurrentIndex(i);
            break;
        }
    }

    return MV_OK;
}

int MainWindow::GetTriggerMode()
{
    MVCC_ENUMVALUE stEnumTriggerModeValue = { 0 };
    MVCC_ENUMENTRY stEnumTriggerModeEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("TriggerMode", &stEnumTriggerModeValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->TriggerModeBox->clear();
    for (int i = 0; i < stEnumTriggerModeValue.nSupportedNum; i++)
    {
        memset(&stEnumTriggerModeEntry, 0, sizeof(stEnumTriggerModeEntry));
        stEnumTriggerModeEntry.nValue = stEnumTriggerModeValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("TriggerMode", &stEnumTriggerModeEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumTriggerModeEntry.chSymbolic);

        ui->TriggerModeBox->addItem(qstrSymbolic);
    }

    // 设置当前值
    for (int i = 0; i < stEnumTriggerModeValue.nSupportedNum; i++)
    {
        if (stEnumTriggerModeValue.nCurValue == stEnumTriggerModeValue.nSupportValue[i])
        {
            ui->TriggerModeBox->setCurrentIndex(i);
            break;
        }
    }

    return MV_OK;
}

int MainWindow::GetExposureTime()
{
    MVCC_FLOATVALUE stFloatValue = {0};

    int nRet = m_MyCamera->GetFloatValue("ExposureTime", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double m_ExposureEdit = stFloatValue.fCurValue;
    ui->ExposureEdit->setText(QString::number(m_ExposureEdit));

    return MV_OK;
}

int MainWindow::SetExposureTime()
{
    m_MyCamera->SetEnumValue("ExposureAuto", MV_EXPOSURE_AUTO_MODE_OFF);

    return m_MyCamera->SetFloatValue("ExposureTime", ui->ExposureEdit->text().toFloat());
}

int MainWindow::GetPreampGain()
{
    MVCC_ENUMVALUE stEnumPreampGainValue = { 0 };
    MVCC_ENUMENTRY stEnumPreampGainEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("PreampGain", &stEnumPreampGainValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->PreampGainBox->clear();
    for (int i = 0; i < stEnumPreampGainValue.nSupportedNum; i++)
    {
        memset(&stEnumPreampGainEntry, 0, sizeof(stEnumPreampGainEntry));
        stEnumPreampGainEntry.nValue = stEnumPreampGainValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("PreampGain", &stEnumPreampGainEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumPreampGainEntry.chSymbolic);

        ui->PreampGainBox->addItem(qstrSymbolic);

        m_mapPreampGain.insert(std::pair<QString, int>(qstrSymbolic, stEnumPreampGainEntry.nValue));
    }

    // 设置当前值
    for (int i = 0; i < stEnumPreampGainValue.nSupportedNum; i++)
    {
        if (stEnumPreampGainValue.nCurValue == stEnumPreampGainValue.nSupportValue[i])
        {
            ui->PreampGainBox->setCurrentIndex(i);
            break;
        }
    }

    m_bPreampGain = true;

    return MV_OK;
}

int MainWindow::GetDigitalGain()
{
    MVCC_FLOATVALUE stFloatValue = {0};

    int nRet = m_MyCamera->GetFloatValue("DigitalShift", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double digitalGain = stFloatValue.fCurValue;
    ui->DigitalGainEdit->setText(QString::number(digitalGain));

    return MV_OK;
}

int MainWindow::SetDigitalGain()
{
    // 设置增益前先把增益使能开关打开，失败无需返回
    m_MyCamera->SetBoolValue("DigitalShiftEnable", TRUE);

    return m_MyCamera->SetFloatValue("DigitalShift", ui->DigitalGainEdit->text().toFloat());
}

int MainWindow::GetAcquisitionLineRateEnable()
{
    bool bAcquisitionLineRateEnable = false;
    int nRet = m_MyCamera->GetBoolValue("AcquisitionLineRateEnable", &bAcquisitionLineRateEnable);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    if (true == bAcquisitionLineRateEnable)
    {
        ui->AcquisitionLineRateEnableBox->setChecked(true);
    }
    else
    {
        ui->AcquisitionLineRateEnableBox->setChecked(false);
    }

    return MV_OK;
}

int MainWindow::GetAcquisitionLineRate()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("AcquisitionLineRate", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double AcquisitionLineRate = stIntValue.nCurValue;
    ui->AcquisitionLineRateEdit->setText(QString::number(AcquisitionLineRate));
    m_bAcquisitionLineRate = true;

    return MV_OK;
}

int MainWindow::GetResultingLineRate()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("ResultingLineRate", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double ResultingLineRate = stIntValue.nCurValue;
    ui->ResultLineRateEdit->setText(QString::number(ResultingLineRate));

    return MV_OK;
}

int MainWindow::SetAcquisitionLineRate()
{
    return m_MyCamera->SetIntValue("AcquisitionLineRate", ui->AcquisitionLineRateEdit->text().toInt());
}

int MainWindow::GetResultingFrameRate()
{
    MVCC_FLOATVALUE stFloatValue = { 0 };

    int nRet = m_MyCamera->GetFloatValue("ResultingFrameRate", &stFloatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }
    double ResultingFrameRate = stFloatValue.fCurValue;
    ui->ResultFrameRateEdit->setText(QString::number(ResultingFrameRate));

    return MV_OK;
}

int MainWindow::GetImageCompressionMode()
{
    MVCC_ENUMVALUE stEnumImageCompressionModeValue = { 0 };
    MVCC_ENUMENTRY stEnumImageCompressionModeEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("ImageCompressionMode", &stEnumImageCompressionModeValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->HBFormatBox->clear();
    for (int i = 0; i < stEnumImageCompressionModeValue.nSupportedNum; i++)
    {
        memset(&stEnumImageCompressionModeEntry, 0, sizeof(stEnumImageCompressionModeEntry));
        stEnumImageCompressionModeEntry.nValue = stEnumImageCompressionModeValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("ImageCompressionMode", &stEnumImageCompressionModeEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumImageCompressionModeEntry.chSymbolic);

        ui->HBFormatBox->addItem(qstrSymbolic);
    }

    // 设置当前值
    for (int i = 0; i < stEnumImageCompressionModeValue.nSupportedNum; i++)
    {
        if (stEnumImageCompressionModeValue.nCurValue == stEnumImageCompressionModeValue.nSupportValue[i])
        {
            ui->HBFormatBox->setCurrentIndex(i);
            break;
        }
    }

    m_HBMode = TRUE;

    return MV_OK;
}

int MainWindow::GetTriggerSource()
{
    MVCC_ENUMVALUE stEnumTriggerSourceValue = { 0 };
    MVCC_ENUMENTRY stEnumTriggerSourceEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("TriggerSource", &stEnumTriggerSourceValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->TriggerSourceBox->clear();
    for (int i = 0; i < stEnumTriggerSourceValue.nSupportedNum; i++)
    {
        memset(&stEnumTriggerSourceEntry, 0, sizeof(stEnumTriggerSourceEntry));
        stEnumTriggerSourceEntry.nValue = stEnumTriggerSourceValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("TriggerSource", &stEnumTriggerSourceEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumTriggerSourceEntry.chSymbolic);

        ui->TriggerSourceBox->addItem(qstrSymbolic);

        m_mapTriggerSource.insert(std::pair<QString, int>(qstrSymbolic, stEnumTriggerSourceEntry.nValue));
    }

    // 设置当前值
    for (int i = 0; i < stEnumTriggerSourceValue.nSupportedNum; i++)
    {
        if (stEnumTriggerSourceValue.nCurValue == stEnumTriggerSourceValue.nSupportValue[i])
        {
            ui->TriggerSourceBox->setCurrentIndex(i);
            break;
        }
    }

    QString strTriggerSource = ui->TriggerSourceBox->currentText();

    QString cStrTriggerSelector = ui->TriggerSelectBox->currentText();

    QString cStrTriggerMode = ui->TriggerModeBox->currentText();

    if ("FrameBurstStart" == cStrTriggerSelector &&cStrTriggerMode == "On" && "Software" == strTriggerSource)
    {
        m_TriggerModeCheck = true;
    }

    return MV_OK;
}

int MainWindow::GetPixelFormat()
{
    MVCC_ENUMVALUE stEnumPixelFormatValue = { 0 };
    MVCC_ENUMENTRY stEnumPixelFormatEntry = { 0 };

    int nRet = m_MyCamera->GetEnumValue("PixelFormat", &stEnumPixelFormatValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    ui->PixelFormatBox->clear();
    for (int i = 0; i < stEnumPixelFormatValue.nSupportedNum; i++)
    {
        memset(&stEnumPixelFormatEntry, 0, sizeof(stEnumPixelFormatEntry));
        stEnumPixelFormatEntry.nValue = stEnumPixelFormatValue.nSupportValue[i];
        m_MyCamera->GetEnumEntrySymbolic("PixelFormat", &stEnumPixelFormatEntry);

        QString qstrSymbolic = QString::fromLatin1(stEnumPixelFormatEntry.chSymbolic);

        ui->PixelFormatBox->addItem(qstrSymbolic);

        m_mapPixelFormat.insert(std::pair<QString, int>(qstrSymbolic, stEnumPixelFormatEntry.nValue));
    }

    // 设置当前值
    for (int i = 0; i < stEnumPixelFormatValue.nSupportedNum; i++)
    {
        if (stEnumPixelFormatValue.nCurValue == stEnumPixelFormatValue.nSupportValue[i])
        {
            ui->PixelFormatBox->setCurrentIndex(i);
            break;
        }
    }

    return MV_OK;
}

int MainWindow::GetWidthHeight()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("Width", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double Width = stIntValue.nCurValue;
    ui->WidthEdit->setText(QString::number(Width));

    nRet = m_MyCamera->GetIntValue("Height", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double Height = stIntValue.nCurValue;
    ui->HeightEdit->setText(QString::number(Height));

    return MV_OK;

}

int MainWindow::SetWidthHeight()
{
    int nRet =  m_MyCamera->SetIntValue("Width", ui->WidthEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }

    nRet =  m_MyCamera->SetIntValue("Height", ui->HeightEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }
    return MV_OK;
}

int MainWindow::GetAcquisitionBurstFrameCount()
{
    MVCC_INTVALUE_EX stIntValue = { 0 };

    int nRet = m_MyCamera->GetIntValue("AcquisitionBurstFrameCount", &stIntValue);
    if (MV_OK != nRet)
    {
        return nRet;
    }

    double temp = stIntValue.nCurValue;
    ui->AcquisitionBurstFrameCountEdit->setText(QString::number(temp));

    return MV_OK;
}

int MainWindow::SetAcquisitionBurstFrameCount()
{
    int nRet =  m_MyCamera->SetIntValue("AcquisitionBurstFrameCount", ui->AcquisitionBurstFrameCountEdit->text().toInt());
    if (MV_OK != nRet)
    {
        return nRet;
    }
    return MV_OK;
}

void __stdcall MainWindow::ImageCallBack(unsigned char * pData, MV_FRAME_OUT_INFO_EX* pFrameInfo, void* pUser)
{
    MainWindow* pThis = (MainWindow*)pUser;

    // QImage img = QImage(pData, pFrameInfo->nWidth,pFrameInfo->nHeight,QImage::Format_RGB888);
    // auto pixmap = QPixmap::fromImage(img);
    // pThis->ui->PicLabel->setPixmap(pixmap.scaled(pThis->ui->PicLabel->width(),pThis->ui->PicLabel->height(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    if (NULL ==  pFrameInfo ||  NULL ==  pData)
    {
        qDebug()<<"ImageCallBackEx Input Param invalid!";
        return;
    }

    int nRet = ArrayQueue::OK;
    nRet = pThis->m_queue->push(pFrameInfo->nFrameNum, pFrameInfo->nExtendWidth, pFrameInfo->nExtendHeight, pData, pFrameInfo->nFrameLenEx);
    if (ArrayQueue::OK != nRet)
    {
        qDebug() << "Add Image to list failed!";
    }
    else
    {
        // qDebug() << "Add Image to list success!";
    }

    return;
}

void MainWindow::on_SearchDevice_clicked()
{
    QString DeviceStr;
    ui->DeviceStrBox->clear();
    memset(&m_stDevList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));

    // 枚举子网内所有设备
    int nRet = CMvCamera::EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE | MV_GENTL_CAMERALINK_DEVICE ,&m_stDevList);

    if (MV_OK != nRet)
    {
        return;
    }

    if (m_stDevList.nDeviceNum == 0)
    {
        QMessageBox::critical(this,"device","No device!");
        return;
    }

    // 将值加入到信息列表框中并显示出来
    for (unsigned int i = 0; i < m_stDevList.nDeviceNum; i++)
    {
        MV_CC_DEVICE_INFO* pDeviceInfo = m_stDevList.pDeviceInfo[i];
        if (NULL == pDeviceInfo)
        {
            continue;
        }

        if (pDeviceInfo->nTLayerType == MV_GIGE_DEVICE)
        {
            int nIp1 = ((m_stDevList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.nCurrentIp & 0xff000000) >> 24);
            int nIp2 = ((m_stDevList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.nCurrentIp & 0x00ff0000) >> 16);
            int nIp3 = ((m_stDevList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.nCurrentIp & 0x0000ff00) >> 8);
            int nIp4 = (m_stDevList.pDeviceInfo[i]->SpecialInfo.stGigEInfo.nCurrentIp & 0x000000ff);

            char strUserName[256] = {0};
            sprintf_s(strUserName, 256, "%s %s (%s)",
                      pDeviceInfo->SpecialInfo.stGigEInfo.chManufacturerName,
                      pDeviceInfo->SpecialInfo.stGigEInfo.chModelName,
                      pDeviceInfo->SpecialInfo.stGigEInfo.chSerialNumber);

            DeviceStr= QString("[%1]GigE: %2  %3.%4.%5.%6")
                         .arg( QString::number(i)).
                     arg(strUserName).
                     arg(QString::number(nIp1)).
                     arg(QString::number(nIp2)).
                     arg(QString::number(nIp3)).
                     arg( QString::number(nIp4));

            ui->DeviceStrBox->addItem(DeviceStr);
        }

    }

    ui->DeviceStrBox->setCurrentIndex(0);

    ui->DeviceOpen->setEnabled(true);

}

void MainWindow::on_DeviceOpen_clicked()
{
    if (true == m_OpenDevice || NULL != m_MyCamera)
    {
        return;
    }

    // int nIndex = m_DeviceCombo;
    int nIndex = ui->DeviceStrBox->currentIndex();

    if ((nIndex < 0) | (nIndex >= MV_MAX_DEVICE_NUM))
    {
        QMessageBox::critical(this,"error","Please select device!");
        return;
    }

    // 由设备信息创建设备实例
    if (NULL == m_stDevList.pDeviceInfo[nIndex])
    {
        QMessageBox::critical(this,"error","Device does not exist!");
        return;
    }

    // 创建相机实例
    m_MyCamera = new CMvCamera;
    if (NULL == m_MyCamera)
    {
        return;
    }

    // 开启相机
    int nRet = m_MyCamera->Open(m_stDevList.pDeviceInfo[nIndex]);
    if (MV_OK != nRet)
    {
        delete m_MyCamera;
        m_MyCamera = NULL;
        QMessageBox::critical(this,"error","Open Fail!");
        return;
    }

    // 探测网络最佳包大小(只对GigE相机有效)
    if (m_stDevList.pDeviceInfo[nIndex]->nTLayerType == MV_GIGE_DEVICE)
    {
        unsigned int nPacketSize = 0;
        nRet = m_MyCamera->GetOptimalPacketSize(&nPacketSize);
        if (nRet == MV_OK)
        {
            nRet = m_MyCamera->SetIntValue("GevSCPSPacketSize",nPacketSize);
            if(nRet != MV_OK)
            {
                QMessageBox::critical(this,"error","Warning: Set Packet Size fail!");
            }
        }
        else
        {
            QMessageBox::critical(this,"error","Warning: Get Packet Size fail!");
        }
    }
    m_OpenDevice = true;

    // 获取所有参数
    on_GetPara_clicked();

    initialflag = true;

    // 设置曝光时间
    nRet = SetExposureTime();

    // 设备打开后启用所有控件
    setDeviceControlsEnabled(true);
    setParameterControlsEnabled(true);
    setAcquisitionControlsEnabled(true);
}

/**
 * @brief 关闭设备
 * 释放相机资源，清理状态，禁用所有控件
 */
void MainWindow::on_DeviceClose_clicked()
{
    // 清理映射表和状态标志
    m_mapPixelFormat.clear();
    m_mapPreampGain.clear();
    m_mapTriggerSource.clear();
    m_TriggerModeCheck = false;
    m_bAcquisitionLineRate = false;
    m_bPreampGain = false;
    m_HBMode = false;

    // 关闭并释放相机
    if (m_MyCamera)
    {
        m_MyCamera->Close();
        delete m_MyCamera;
        m_MyCamera = nullptr;
    }

    // 重置状态标志
    m_OpenDevice = false;
    m_StartGrabbing = false;

    // 禁用所有控件
    setDeviceControlsEnabled(false);
    setParameterControlsEnabled(false);
    setAcquisitionControlsEnabled(false);
}

/**
 * @brief 获取相机参数
 * 从相机读取所有当前参数并更新到界面
 */
void MainWindow::on_GetPara_clicked()
{
    // 暂时禁用事件处理，避免获取参数时触发设置操作
    initialflag = false;

    int nRet = GetTriggerSelector();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Selector Fail!");
    }

    nRet = GetTriggerMode();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Mode Fail!");
    }

    nRet = GetTriggerSource();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Source Fail!");
    }

    nRet = GetExposureTime();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Exposure time Fail!");
    }

    nRet = GetPreampGain();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Preamp Gain Fail!");
    }

    nRet = GetDigitalGain();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Digital Gain Fail!");
    }

    nRet = GetAcquisitionLineRateEnable();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionLineRateEnable Fail!");
    }

    nRet = GetAcquisitionLineRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionLineRate Fail!");
    }

    nRet = GetResultingLineRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ResultingLineRate Fail!");
    }

    nRet = GetResultingFrameRate();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ResultingFrameRate Fail!");
    }

    nRet = GetPixelFormat();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Pixel Format Fail!");
    }

    nRet = GetImageCompressionMode();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get ImageCompressionMode Fail!");
    }

    nRet = GetWidthHeight();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get WidthHeight Fail!");
    }

    nRet = GetAcquisitionBurstFrameCount();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get AcquisitionBurstFrameCount Fail!");
    }

    // 参数获取完成，重新启用事件处理
    initialflag = true;
}

/**
 * @brief 设置相机参数
 * 将界面上的参数设置到相机
 */
void MainWindow::on_SetPara_clicked()
{
    bool allSucceeded = true;

    int nRet = SetExposureTime();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set Exposure Time Fail!");
    }

    nRet = SetDigitalGain();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set DigitalGain Fail!");
    }

    nRet = SetWidthHeight();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set WidthHeight Fail!");
    }

    nRet = SetAcquisitionBurstFrameCount();
    if (nRet != MV_OK)
    {
        allSucceeded = false;
        QMessageBox::critical(this, "error", "Set AcquisitionBurstFrameCount Fail!");
    }

    // 如果启用了行频控制，设置行频参数
    if (m_bAcquisitionLineRate)
    {
        nRet = SetAcquisitionLineRate();
        if (nRet != MV_OK)
        {
            allSucceeded = false;
            QMessageBox::critical(this, "error", "Set AcquisitionLineRate Fail!");
        }
    }

    if (allSucceeded)
    {
        QMessageBox::information(this, "success", "Set Parameter Succeed!");
    }
}

void MainWindow::on_StartGrab_clicked()
{
    if (false == m_OpenDevice || true == m_StartGrabbing || NULL == m_MyCamera)
    {
        return;
    }

    ui->PicLabel->clear();

    // 获取图像数据大小
    MVCC_INTVALUE_EX stIntEx = {0};
    int nRet = m_MyCamera->GetIntValue("PayloadSize", &stIntEx);
    if (MV_OK != nRet)
    {
        int nWidth = ui->WidthEdit->text().toInt();
        int nHeight = ui->HeightEdit->text().toInt();

        m_nImageSize =  nHeight*nWidth*3;
    }
    else
    {
        m_nImageSize =  stIntEx.nCurValue;
    }

    // 初始化队列
    m_queue = new (std::nothrow)ArrayQueue();
    if (!m_queue)
    {
        QMessageBox::critical(this, "error", "Failed to create image queue!");
        return;
    }

    nRet = m_queue->init(MAX_QUEUE_COUNT, m_nImageSize);
    if (ArrayQueue::OK != nRet)
    {
        QMessageBox::critical(this, "error", "ArrayQueue init fail!");
        delete m_queue;
        m_queue = nullptr;
        return;
    }

    // 注册图像回调函数
    nRet = m_MyCamera->RegisterImageCallBack(ImageCallBack, this);
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Register callback function failed!");
        return;
    }

    m_StartGrabbing = true;

    // 开始采集
    nRet = m_MyCamera->StartGrabbing();
    if (MV_OK != nRet)
    {
        m_ThreadState = false;
        QMessageBox::critical(this, "error", "Start grabbing fail!");
        return;
    }

    // 启动图像处理线程
    m_ThreadState = true;
    m_ProcessThread = new ProcessThread(this);

    // 连接ProcessThread的检测耗时信号
    connect(m_ProcessThread, &ProcessThread::detectionTimeUpdated,
            this, &MainWindow::updateDetectionTime,
            Qt::QueuedConnection);

    // 连接缺陷信息相关信号（线程安全）
    connect(m_ProcessThread, &ProcessThread::addDefectInfo,
            this, [this](const QString& defectInfo) {
                ui->DefectListWidget->addItem(defectInfo);
            }, Qt::QueuedConnection);

    connect(m_ProcessThread, &ProcessThread::clearDefectList,
            this, [this]() {
                ui->DefectListWidget->clear();
            }, Qt::QueuedConnection);

    m_ProcessThread->start();

    // 如果是软触发模式，启用软触发按钮
    QString triggerSource = ui->TriggerSourceBox->currentText();
    ui->SingleSoftTrigger->setEnabled(triggerSource == "Software" && m_TriggerModeCheck);

    // 刷新参数显示
    on_GetPara_clicked();

    // 采集中，禁用大部分控件
    setGrabbingControlsEnabled(true);
}

/**
 * @brief 停止采集
 * 停止图像采集，释放处理线程和队列资源
 */
void MainWindow::on_StopGrab_clicked()
{
    if (!m_OpenDevice || !m_StartGrabbing || !m_MyCamera)
    {
        return;
    }

    // 停止图像处理线程
    if (m_ThreadState)
    {
        m_ThreadState = false;
        m_ProcessThread->quit();
        m_ProcessThread->wait();
        delete m_ProcessThread;
        m_ProcessThread = nullptr;
    }

    // 停止采集
    int nRet = m_MyCamera->StopGrabbing();
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Stop grabbing fail!");
        return;
    }

    // 注销图像回调
    nRet = m_MyCamera->RegisterImageCallBack(nullptr, nullptr);
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Unregister Image CallBack fail!");
        return;
    }

    // 释放队列
    if (m_queue)
    {
        delete m_queue;
        m_queue = nullptr;
    }

    m_StartGrabbing = false;

    // 刷新参数显示
    on_GetPara_clicked();

    // 停止采集后，恢复控件使能
    setGrabbingControlsEnabled(false);
    ui->SingleSoftTrigger->setEnabled(false);
}

/**
 * @brief 执行单次软触发
 * 在软触发模式下，手动触发一次图像采集
 */
void MainWindow::on_SingleSoftTrigger_clicked()
{
    if (!m_StartGrabbing)
    {
        return;
    }

    m_MyCamera->CommandExecute("TriggerSoftware");
}

/**
 * @brief 触发选项切换处理
 * 设置触发选项为帧突发开始或行开始，并更新软触发按钮状态
 */
void MainWindow::on_TriggerSelectBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString triggerSelector = ui->TriggerSelectBox->currentText();
    int nRet = MV_OK;

    if ("FrameBurstStart" == triggerSelector)
    {
        nRet = m_MyCamera->SetEnumValue("TriggerSelector", FRAMEBURSTSTART);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set TriggerSelector FrameBurstStart fail!");
            return;
        }

        // 检查是否应启用软触发按钮
        QString triggerMode = ui->TriggerModeBox->currentText();
        QString triggerSource = ui->TriggerSourceBox->currentText();
        m_TriggerModeCheck = (triggerMode == "On" && triggerSource == "Software");
    }
    else if (triggerSelector == "LineStart")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerSelector", LINESTART);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set TriggerSelector LineStart fail!");
            return;
        }
        m_TriggerModeCheck = false;
    }

    // 刷新触发源显示
    nRet = GetTriggerSource();
    if (nRet != MV_OK)
    {
        QMessageBox::critical(this, "error", "Get Trigger Source Fail!");
    }
}

/**
 * @brief 触发模式切换处理
 * 开启或关闭触发模式，并更新软触发按钮状态
 */
void MainWindow::on_TriggerModeBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString triggerMode = ui->TriggerModeBox->currentText();
    int nRet = MV_OK;

    if (triggerMode == "On")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerMode", MV_TRIGGER_MODE_ON);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set Trigger Mode fail!");
            return;
        }

        // 检查是否应启用软触发按钮
        QString triggerSelector = ui->TriggerSelectBox->currentText();
        QString triggerSource = ui->TriggerSourceBox->currentText();
        m_TriggerModeCheck = (triggerSelector == "FrameBurstStart" && triggerSource == "Software");
    }
    else if (triggerMode == "Off")
    {
        nRet = m_MyCamera->SetEnumValue("TriggerMode", MV_TRIGGER_MODE_OFF);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set Trigger Mode fail!");
            return;
        }
        m_TriggerModeCheck = false;
    }
}

/**
 * @brief 触发源切换处理
 * 根据下拉框选择设置相应的触发源
 */
void MainWindow::on_TriggerSourceBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    m_TriggerModeCheck = false;
    QString triggerSource = ui->TriggerSourceBox->currentText();

    // 使用 map 查找对应的触发源枚举值
    auto it = m_mapTriggerSource.find(triggerSource);
    if (it != m_mapTriggerSource.end())
    {
        // 设置触发源
        int nRet = m_MyCamera->SetEnumValue("TriggerSource", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error",
                QString("Set Trigger Source %1 fail!").arg(triggerSource));
            return;
        }

        // 如果是软触发模式，检查是否需要启用软触发按钮
        if (triggerSource == "Software")
        {
            QString triggerSelector = ui->TriggerSelectBox->currentText();
            QString triggerMode = ui->TriggerModeBox->currentText();

            if (triggerSelector == "FrameBurstStart" && triggerMode == "On")
            {
                m_TriggerModeCheck = true;
            }
        }
    }
    else
    {
        QMessageBox::critical(this, "error",
            QString("Unknown trigger source: %1").arg(triggerSource));
    }
}

/**
 * @brief 像素格式切换处理
 * 设置相机像素格式，并在HB模式下刷新图像压缩模式
 */
void MainWindow::on_PixelFormatBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString pixelFormat = ui->PixelFormatBox->currentText();

    // 使用 map 查找对应的像素格式枚举值
    auto it = m_mapPixelFormat.find(pixelFormat);
    if (it != m_mapPixelFormat.end())
    {
        int nRet = m_MyCamera->SetEnumValue("PixelFormat", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set PixelFormat fail!");
            return;
        }
    }

    // HB模式下需要刷新图像压缩模式
    if (m_HBMode)
    {
        int nRet = GetImageCompressionMode();
        if (nRet != MV_OK)
        {
            QMessageBox::critical(this, "error", "Get Image Compression Mode Fail!");
        }
    }
}

/**
 * @brief 前置增益切换处理
 * 设置相机的前置增益值
 */
void MainWindow::on_PreampGainBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString preampGain = ui->PreampGainBox->currentText();

    // 使用 map 查找对应的增益枚举值
    auto it = m_mapPreampGain.find(preampGain);
    if (it != m_mapPreampGain.end())
    {
        int nRet = m_MyCamera->SetEnumValue("PreampGain", it->second);
        if (MV_OK != nRet)
        {
            QMessageBox::critical(this, "error", "Set PreampGain fail!");
        }
    }
}

/**
 * @brief HB格式切换处理
 * 开启或关闭图像压缩模式（HB模式）
 */
void MainWindow::on_HBFormatBox_currentIndexChanged()
{
    if (!initialflag)
    {
        return;
    }

    QString compressionMode = ui->HBFormatBox->currentText();
    int nRet = MV_OK;

    if (compressionMode == "Off")
    {
        nRet = m_MyCamera->SetEnumValue("ImageCompressionMode", IMAGE_COMPRESSION_MODE_OFF);
    }
    else if (compressionMode == "HB")
    {
        nRet = m_MyCamera->SetEnumValue("ImageCompressionMode", IMAGE_COMPRESSION_MODE_HB);
    }

    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Set Image Compression Mode fail!");
    }
}

/**
 * @brief 行频使能状态切换处理
 * 启用或禁用采集行频控制
 */
void MainWindow::on_AcquisitionLineRateEnableBox_stateChanged()
{
    if (!initialflag)
    {
        return;
    }

    bool enabled = ui->AcquisitionLineRateEnableBox->isChecked();

    // 根据复选框状态控制AcquisitionLineRateEdit的使能状态
    ui->AcquisitionLineRateEdit->setEnabled(enabled && !m_StartGrabbing);

    int nRet = m_MyCamera->SetBoolValue("AcquisitionLineRateEnable", enabled);
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Set Acquisition LineRate Enable fail!");
    }
}

/**
 * @brief 选择图像保存路径
 * 弹出目录选择对话框，设置图像保存路径
 */
void MainWindow::on_SelectSavePath_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "选择保存目录",
        QCoreApplication::applicationDirPath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty())
    {
        ui->imgSavePathEdit->setText(QDir::toNativeSeparators(dir));
    }
}

/**
 * @brief 选择模型文件路径
 * 弹出文件选择对话框，选择 TensorRT YOLO 模型文件 (.engine)
 */
void MainWindow::on_SelectModelPath_clicked()
{
    // 获取当前模型路径作为默认打开目录
    QString currentPath = ui->ModelPathEdit->text();
    QString defaultDir;

    if (!currentPath.isEmpty() && QFileInfo(currentPath).exists())
    {
        defaultDir = QFileInfo(currentPath).absolutePath();
    }
    else
    {
        defaultDir = QCoreApplication::applicationDirPath();
    }

    // 打开文件选择对话框
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "选择 YOLO 模型文件",
        defaultDir,
        "TensorRT Engine Files (*.engine);;All Files (*.*)"
    );

    if (!fileName.isEmpty())
    {
        // 更新 UI 显示
        ui->ModelPathEdit->setText(QDir::toNativeSeparators(fileName));

        // 更新成员变量
        m_modelPath = fileName;

        // 尝试重新加载模型
        bool loadSuccess = false;
        QFileInfo modelFile(m_modelPath);

        if (modelFile.exists())
        {
            try
            {
                // 初始化推理选项
                trtyolo::InferOption option;
                option.enableSwapRB();  // 启用 RGB 通道交换（从 RGB 到 BGR）

                // 创建检测模型（会自动替换旧模型）
                m_yoloDetector = std::make_unique<trtyolo::DetectModel>(m_modelPath.toStdString(), option);

                qDebug() << "YOLO 检测器重新加载成功，模型路径:" << m_modelPath;

                // 设置 DetectCheckBox 可用且保持之前的选中状态
                bool wasChecked = ui->DetectCheckBox->isChecked();
                ui->DetectCheckBox->setEnabled(true);

                // 如果之前是选中的，保持启用检测
                if (wasChecked)
                {
                    ui->DetectCheckBox->setChecked(true);
                    m_enableDefectDetection = true;
                }

                loadSuccess = true;

                QMessageBox::information(this, "模型加载",
                    "YOLO 模型加载成功！\n路径：" + m_modelPath);
            }
            catch (const std::exception& e)
            {
                qCritical() << "YOLO 检测器重新加载失败:" << e.what();
                m_enableDefectDetection = false;

                // 禁用 DetectCheckBox
                ui->DetectCheckBox->setEnabled(false);
                ui->DetectCheckBox->setChecked(false);

                QMessageBox::critical(this, "模型加载失败",
                    "YOLO 模型加载失败！\n错误信息：" + QString(e.what()));
            }
        }
        else
        {
            qWarning() << "YOLO 模型文件不存在:" << m_modelPath;
            m_enableDefectDetection = false;

            // 禁用 DetectCheckBox
            ui->DetectCheckBox->setEnabled(false);
            ui->DetectCheckBox->setChecked(false);

            QMessageBox::warning(this, "文件不存在",
                "所选模型文件不存在！\n路径：" + m_modelPath);
        }
        updateModelStatus();
    }
}

/**
 * @brief 保存相机参数（功能未实现）
 * 预留接口，用于将当前参数保存到相机
 */
void MainWindow::on_SavePara_clicked()
{
    // 功能暂未实现
    // int nRet = m_MyCamera->SavePara();
    // if (MV_OK != nRet)
    // {
    //     QMessageBox::critical(this, "error", "Save parameters fail!");
    // }
}

/**
 * @brief 加载相机参数
 * 从相机加载已保存的参数配置
 */
void MainWindow::on_LoadPara_clicked()
{
    int nRet = m_MyCamera->LoadPara();
    if (MV_OK != nRet)
    {
        QMessageBox::critical(this, "error", "Load parameters fail!");
    }
}

// ==================== PLC通信功能 ====================

/**
 * @brief 缺陷检测复选框状态改变槽函数
 * @param state 复选框状态（Qt::Unchecked=0, Qt::Checked=2）
 *
 * 根据复选框状态控制缺陷检测功能的启用/禁用：
 * - 选中：启用缺陷检测（前提是YOLO检测器已初始化）
 * - 未选中：禁用缺陷检测
 */
void MainWindow::on_DetectCheckBox_stateChanged(int state)
{
    if (state == Qt::Checked)
    {
        // 检查YOLO检测器是否已初始化
        if (m_yoloDetector)
        {
            m_enableDefectDetection = true;
            qDebug() << "缺陷检测已启用";
        }
        else
        {
            m_enableDefectDetection = false;
            ui->DetectCheckBox->setChecked(false);
            QMessageBox::warning(this, "缺陷检测",
                "YOLO检测器未初始化！\n"
                "请检查模型文件路径是否正确：\n" + m_modelPath);
            qWarning() << "无法启用缺陷检测：YOLO检测器未初始化";
        }
    }
    else
    {
        m_enableDefectDetection = false;
        qDebug() << "缺陷检测已禁用";
    }
}

/**
 * @brief 更新检测耗时显示
 * @param elapsedMs 检测耗时（毫秒）
 *
 * 在DetectTimeLabel上显示检测耗时
 */
void MainWindow::updateDetectionTime(qint64 elapsedMs)
{
    if (ui && ui->DetectTimeLabel)
    {
        ui->DetectTimeLabel->setText(QString("%1 ms").arg(elapsedMs));
    }
}

// ==================== 光源控制功能实现 ====================

/**
 * @brief 初始化串口列表
 * 扫描系统中所有可用的串口并添加到下拉框中
 */
void MainWindow::initLightPortList()
{
    ui->PortCombobox->clear();

    // 扫描所有可用串口
    QList<QSerialPortInfo> portList = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : portList)
    {
        ui->PortCombobox->addItem(info.portName());
    }

    // 如果没有检测到串口，显示提示信息
    if (portList.isEmpty())
    {
        ui->PortCombobox->setCurrentText("未检测到可用串口");
    }
}

/**
 * @brief 更新光源控件使能状态
 * @param isPortOpen true-串口已打开, false-串口未打开
 *
 * 串口打开时启用光源控制按钮，关闭时禁用
 */
void MainWindow::updateLightControlsEnabled(bool isPortOpen)
{
    ui->PortConnect->setEnabled(!isPortOpen);
    ui->PortDisconnect->setEnabled(isPortOpen);
    ui->LightOn->setEnabled(isPortOpen);
    ui->LightOff->setEnabled(isPortOpen);
    ui->LightnessEdit->setEnabled(isPortOpen);
    ui->LightnessSet->setEnabled(isPortOpen);
    ui->LightStatusGet->setEnabled(isPortOpen);
}

/**
 * @brief 串口连接按钮槽函数
 * 根据下拉框选择的串口号打开串口连接
 */
void MainWindow::on_PortConnect_clicked()
{
    QString portName = ui->PortCombobox->currentText();

    // 验证串口选择
    if (portName == "请选择串口" || portName == "未检测到可用串口" || portName.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请先选择有效的串口");
        return;
    }

    // 尝试打开串口
    if (m_lightController->initSerial(portName))
    {
        updateLightControlsEnabled(true);
        setStatusChip(ui->LightStatusLabel, "已连接", "busy");
    }
    else
    {
        QMessageBox::critical(this, "错误", "串口打开失败");
    }
}

/**
 * @brief 串口断开按钮槽函数
 * 关闭当前打开的串口连接
 */
void MainWindow::on_PortDisconnect_clicked()
{
    m_lightController->closeSerial();
    updateLightControlsEnabled(false);
    setStatusChip(ui->LightStatusLabel, "未连接", "off");
}

/**
 * @brief 光源开启按钮槽函数
 * 发送指令将光源设置为常亮状态
 */
void MainWindow::on_LightOn_clicked()
{
    if (m_lightController->setLightOn())
    {
        setStatusChip(ui->LightStatusLabel, "光源: 开启", "ok");
    }
    else
    {
        QMessageBox::warning(this, "错误", "光源开启失败");
    }
}

/**
 * @brief 光源关闭按钮槽函数
 * 发送指令将光源设置为常灭状态
 */
void MainWindow::on_LightOff_clicked()
{
    if (m_lightController->setLightOff())
    {
        setStatusChip(ui->LightStatusLabel, "光源: 关闭", "error");
    }
    else
    {
        QMessageBox::warning(this, "错误", "光源关闭失败");
    }
}

/**
 * @brief 亮度设置按钮槽函数
 * 读取输入框中的亮度值并设置到光源控制器
 */
void MainWindow::on_LightnessSet_clicked()
{
    QString brightnessText = ui->LightnessEdit->text();

    // 检查输入是否为空
    if (brightnessText.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请输入亮度值（0-255）");
        return;
    }

    // 转换为整数并验证范围
    bool ok;
    int brightness = brightnessText.toInt(&ok);
    if (!ok || brightness < 0 || brightness > 255)
    {
        QMessageBox::warning(this, "警告", "亮度值必须在0-255之间");
        return;
    }

    // 设置亮度
    if (!m_lightController->setLightBrightness(brightness))
    {
        QMessageBox::warning(this, "错误", "亮度设置失败");
    }
}

/**
 * @brief 光源状态读取按钮槽函数
 * 查询并显示当前光源的开关状态和亮度值
 */
void MainWindow::on_LightStatusGet_clicked()
{
    // 查询并更新亮度值
    int brightness = m_lightController->queryLightBrightness();
    ui->LightnessEdit->setText(QString::number(brightness));

    // 读取光源开关状态
    bool isOn = m_lightController->isLightOn();
    QString stateText = isOn ? "开启" : "关闭";

    // 更新状态显示
    setStatusChip(ui->LightStatusLabel, QString("光源: %1, 亮度: %2").arg(stateText).arg(brightness), isOn ? "ok" : "error");
}

// ==================== 传送带控制功能实现 ====================

/**
 * @brief 初始化传送带串口列表
 * 扫描系统中所有可用的串口并添加到下拉框中
 */
void MainWindow::initConveyorPortList()
{
    ui->ConveyorPortComboBox->clear();

    // 扫描所有可用串口
    QList<QSerialPortInfo> portList = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : portList)
    {
        ui->ConveyorPortComboBox->addItem(info.portName());
    }

    // 如果没有检测到串口，显示提示信息
    if (portList.isEmpty())
    {
        ui->ConveyorPortComboBox->setCurrentText("未检测到可用串口");
    }
}

/**
 * @brief 更新传送带控件使能状态
 * @param isPortOpen true-串口已打开, false-串口未打开
 *
 * 串口打开时启用传送带控制按钮，关闭时禁用
 */
void MainWindow::updateConveyorControlsEnabled(bool isPortOpen)
{
    ui->ConConnect->setEnabled(!isPortOpen);
    ui->ConDisconnect->setEnabled(isPortOpen);
    ui->ConForward->setEnabled(isPortOpen);
    ui->ConReverse->setEnabled(isPortOpen);
    ui->ConStop->setEnabled(isPortOpen);
}

/**
 * @brief 更新传送带状态显示
 * @param status 传送带状态字符串
 *
 * 根据状态设置不同的显示颜色
 */
void MainWindow::updateConveyorStatus(const QString &status)
{
    // 根据状态选择显示颜色（颜色定义见 .ui 样式表中的 state 属性）
    const char* state = "busy";
    if (status.contains("未连接"))
        state = "off";
    else if (status.contains("正转"))
        state = "ok";
    else if (status.contains("反转"))
        state = "warn";
    else if (status.contains("停止"))
        state = "error";
    setStatusChip(ui->ConSatus, status, state);
}

/**
 * @brief 传送带串口连接按钮槽函数
 * 根据下拉框选择的串口号打开串口连接
 */
void MainWindow::on_ConConnect_clicked()
{
    QString portName = ui->ConveyorPortComboBox->currentText();

    // 验证串口选择
    if (portName == "请选择串口" || portName == "未检测到可用串口" || portName.isEmpty())
    {
        QMessageBox::warning(this, "警告", "请先选择有效的串口");
        return;
    }

    // 尝试打开串口（使用默认波特率9600）
    if (m_conveyorController->initSerial(portName))
    {
        updateConveyorControlsEnabled(true);
        qDebug() << "传送带串口连接成功:" << portName;
    }
    else
    {
        QMessageBox::critical(this, "错误", "传送带串口打开失败");
    }
}

/**
 * @brief 传送带串口断开按钮槽函数
 * 关闭当前打开的串口连接
 */
void MainWindow::on_ConDisconnect_clicked()
{
    m_conveyorController->closeSerial();
    updateConveyorControlsEnabled(false);
    qDebug() << "传送带串口已断开";
}

/**
 * @brief 传送带正转按钮槽函数
 * 发送正转指令到PLC
 */
void MainWindow::on_ConForward_clicked()
{
    if (!m_conveyorController->setForward())
    {
        QMessageBox::warning(this, "警告", "正转命令发送失败");
    }
}

/**
 * @brief 传送带反转按钮槽函数
 * 发送反转指令到PLC
 */
void MainWindow::on_ConReverse_clicked()
{
    if (!m_conveyorController->setReverse())
    {
        QMessageBox::warning(this, "警告", "反转命令发送失败");
    }
}

/**
 * @brief 传送带停止按钮槽函数
 * 发送停止指令到PLC
 */
void MainWindow::on_ConStop_clicked()
{
    if (!m_conveyorController->setStop())
    {
        QMessageBox::warning(this, "警告", "停止命令发送失败");
    }
}
