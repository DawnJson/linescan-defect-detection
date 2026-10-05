#ifndef BRANDING_H
#define BRANDING_H

#include <QString>

/**
 * @brief 品牌信息（标题、单位、登录账号等）
 *
 * 从资源 :/branding/branding.ini 读取。构建时 HIKONCam.pro 只编译一套品牌资源：
 * 存在 branding/local/ 时用本地自定义品牌，否则用 branding/default/（随仓库公开）。
 * 两套资源的前缀都是 /branding，因此代码与 .ui 中的路径无需区分。
 */
struct Branding
{
    QString windowTitle;   // 窗口标题
    QString title;         // 系统名称
    QString tagline;       // 登录页标语
    QString organization;  // 单位名称，为空时不显示
    QString account;       // 内置登录账号
    QString password;      // 内置登录密码

    static const Branding& instance();
};

#endif // BRANDING_H
