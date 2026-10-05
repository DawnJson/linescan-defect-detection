/**
 * @file logindialog.cpp
 * @brief 登录对话框实现
 *
 * 实现账号密码的保存、加载和自动登录功能
 *
 * @author DawnJson
 * @date 2025-11-12
 * @version 1.0
 */

#include "logindialog.h"
#include "ui_logindialog.h"
#include <QMessageBox>
#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QTimer>
#include "branding.h"

/**
 * @brief 获取配置文件路径
 * @return 配置文件的完整路径
 */
QString LoginDialog::getConfigFilePath() const
{
    // 获取程序运行目录
    QString appPath = QCoreApplication::applicationDirPath();

    // 获取上一级目录
    QDir dir(appPath);
    dir.cdUp();  // 切换到上一级目录
    QString parentPath = dir.absolutePath();

    // 配置文件夹路径（在上一级目录下）
    QString configDir = parentPath + "/config";

    // 确保配置文件夹存在
    if (!dir.exists(configDir)) {
        dir.mkpath(configDir);
        qDebug() << "创建配置文件夹：" << configDir;
    }

    // 返回完整的配置文件路径
    QString configFilePath = configDir + "/login.ini";

    return configFilePath;
}

/**
 * @brief 构造函数
 * @param parent 父窗口指针
 */
LoginDialog::LoginDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LoginDialog)
{
    ui->setupUi(this);

    // 品牌信息（标题、标语、单位）来自 :/branding/branding.ini
    const Branding& brand = Branding::instance();
    setWindowTitle(brand.windowTitle);
    ui->brandTitle->setText(brand.title);
    ui->brandTagline->setText(brand.tagline);
    ui->brandFooter->setText(brand.organization);
    ui->brandFooter->setVisible(!brand.organization.isEmpty());

    // 设置密码输入框为密码模式
    ui->PasswordEdit->setEchoMode(QLineEdit::Password);

    // 设置AccountCombobox为可编辑（允许输入新账号）
    ui->AccountCombobox->setEditable(true);

    // 加载保存的账号信息
    loadAccounts();

    // 需要自动登录时延迟到 exec() 事件循环内执行，否则 exec() 会重置对话框结果
    if (ui->AutoLoginBox->isChecked() && ui->AccountCombobox->count() > 0) {
        QTimer::singleShot(0, this, &LoginDialog::performAutoLogin);
    }
}

/**
 * @brief 析构函数
 */
LoginDialog::~LoginDialog()
{
    delete ui;
}

/**
 * @brief 加载保存的账号和密码
 */
void LoginDialog::loadAccounts()
{
    QSettings settings(getConfigFilePath(), QSettings::IniFormat);

    // 读取账号数量
    int accountCount = settings.beginReadArray("Accounts");

    for (int i = 0; i < accountCount; ++i) {
        settings.setArrayIndex(i);
        QString account = settings.value("username").toString();
        QString password = settings.value("password").toString();

        // 保存到映射表
        m_accountMap[account] = password;

        // 添加到下拉框
        ui->AccountCombobox->addItem(account);
    }

    settings.endArray();

    // 加载记住密码和自动登录状态
    bool rememberPassword = settings.value("RememberPassword", false).toBool();
    bool autoLogin = settings.value("AutoLogin", false).toBool();

    ui->RememberBox->setChecked(rememberPassword);
    ui->AutoLoginBox->setChecked(autoLogin);
}

/**
 * @brief 保存当前账号和密码
 */
void LoginDialog::saveCurrentAccount()
{
    QString account = ui->AccountCombobox->currentText().trimmed();
    QString password = ui->PasswordEdit->text();

    if (account.isEmpty()) {
        return;
    }

    QSettings settings(getConfigFilePath(), QSettings::IniFormat);

    // 如果勾选了记住密码，保存账号密码
    if (ui->RememberBox->isChecked()) {
        // 更新映射表
        m_accountMap[account] = password;

        // 保存所有账号
        settings.beginWriteArray("Accounts");
        int index = 0;
        for (auto it = m_accountMap.begin(); it != m_accountMap.end(); ++it) {
            settings.setArrayIndex(index++);
            settings.setValue("username", it.key());
            settings.setValue("password", it.value());
        }
        settings.endArray();
    } else {
        // 不记住密码，清除所有保存的账号
        settings.remove("Accounts");
        m_accountMap.clear();
    }

    // 保存记住密码和自动登录状态
    settings.setValue("RememberPassword", ui->RememberBox->isChecked());
    settings.setValue("AutoLogin", ui->AutoLoginBox->isChecked());

    // 确保立即写入文件
    settings.sync();

    // qDebug() << "配置已保存";
}

/**
 * @brief 执行自动登录
 */
void LoginDialog::performAutoLogin()
{
    if (ui->AccountCombobox->count() == 0) {
        return;
    }

    // 选择第一个账号
    ui->AccountCombobox->setCurrentIndex(0);
    QString account = ui->AccountCombobox->currentText();
    QString password = m_accountMap.value(account, "");

    ui->PasswordEdit->setText(password);

    // 验证并登录
    if (validateLogin(account, password)) {
        qDebug() << "自动登录成功：" << account;
        accept();  // 关闭对话框并返回 Accepted
    } else {
        QMessageBox::warning(this, "自动登录失败", "账号或密码错误，请手动登录");
    }
}

/**
 * @brief 验证用户名和密码
 * @param account 账号
 * @param password 密码
 * @return 验证是否成功
 */
bool LoginDialog::validateLogin(const QString &account, const QString &password)
{
    // 内置账号来自品牌配置 :/branding/branding.ini
    const Branding& brand = Branding::instance();
    if (!brand.account.isEmpty() && account == brand.account && password == brand.password) {
        return true;
    }

    // 允许已保存的账号密码组合
    if (m_accountMap.contains(account) && m_accountMap[account] == password) {
        return true;
    }

    return false;
}

/**
 * @brief 登录按钮点击槽函数
 */
void LoginDialog::on_LoginBtn_clicked()
{
    QString account = ui->AccountCombobox->currentText().trimmed();
    QString password = ui->PasswordEdit->text();

    // 验证输入
    if (account.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入账号");
        ui->AccountCombobox->setFocus();
        return;
    }

    if (password.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入密码");
        ui->PasswordEdit->setFocus();
        return;
    }

    // 验证账号密码
    if (validateLogin(account, password)) {
        // 保存账号信息
        saveCurrentAccount();

        qDebug() << "登录成功：" << account;
        accept();  // 关闭对话框并返回 Accepted
    } else {
        QMessageBox::warning(this, "登录失败", "账号或密码错误，请重新输入");
        ui->PasswordEdit->clear();
        ui->PasswordEdit->setFocus();
    }
}

/**
 * @brief 账号选择变化槽函数
 * @param account 选中的账号
 */
void LoginDialog::on_AccountCombobox_currentTextChanged(const QString &account)
{
    // 当账号改变时，自动填充对应的密码
    if (m_accountMap.contains(account)) {
        ui->PasswordEdit->setText(m_accountMap[account]);
    } else {
        ui->PasswordEdit->clear();
    }
}

/**
 * @brief 记住密码复选框状态变化槽函数
 * @param state 复选框状态
 */
void LoginDialog::on_RememberBox_stateChanged(int state)
{
    // 如果取消记住密码，自动取消自动登录
    if (state == Qt::Unchecked) {
        ui->AutoLoginBox->setChecked(false);
    }
}

/**
 * @brief 自动登录复选框状态变化槽函数
 * @param state 复选框状态
 */
void LoginDialog::on_AutoLoginBox_stateChanged(int state)
{
    // 如果勾选自动登录，自动勾选记住密码
    if (state == Qt::Checked) {
        ui->RememberBox->setChecked(true);
    }
}
