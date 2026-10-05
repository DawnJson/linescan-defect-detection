/**
 * @file logindialog.h
 * @brief 登录对话框
 *
 * 提供用户登录功能，支持账号密码记忆和自动登录
 *
 * @author DawnJson
 * @date 2025-11-12
 * @version 1.0
 */

#ifndef LOGINDIALOG_H
#define LOGINDIALOG_H

#include <QDialog>
#include <QSettings>
#include <QMap>

namespace Ui {
class LoginDialog;
}

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget *parent = nullptr);
    ~LoginDialog();

private slots:
    /**
     * @brief 登录按钮点击槽函数
     */
    void on_LoginBtn_clicked();

    /**
     * @brief 账号选择变化槽函数
     * @param account 选中的账号
     */
    void on_AccountCombobox_currentTextChanged(const QString &account);

    /**
     * @brief 记住密码复选框状态变化槽函数
     * @param state 复选框状态
     */
    void on_RememberBox_stateChanged(int state);

    /**
     * @brief 自动登录复选框状态变化槽函数
     * @param state 复选框状态
     */
    void on_AutoLoginBox_stateChanged(int state);

private:
    Ui::LoginDialog *ui;

    /**
     * @brief 加载保存的账号和密码
     */
    void loadAccounts();

    /**
     * @brief 保存当前账号和密码
     */
    void saveCurrentAccount();

    /**
     * @brief 执行自动登录
     */
    void performAutoLogin();

    /**
     * @brief 验证用户名和密码
     * @param account 账号
     * @param password 密码
     * @return 验证是否成功
     */
    bool validateLogin(const QString &account, const QString &password);

    /**
     * @brief 获取配置文件路径
     * @return 配置文件的完整路径
     */
    QString getConfigFilePath() const;

    // 存储账号和密码的映射表 (账号 -> 密码)
    QMap<QString, QString> m_accountMap;
};

#endif // LOGINDIALOG_H
