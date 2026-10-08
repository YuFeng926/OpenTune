# OpenTune 安装声明文案（唯一来源）

本文件是安装/首次启动「渠道与许可声明」的唯一文案来源，当前与 `Installer/OpenTune_Installer.iss`（v1.8.0 / `[Messages]` 段 `WelcomeLabel1/2`）保持一致。

- 用途：反诈与官方渠道声明 + OpenVPI 声码器权重许可警示。
- 修改约定：改文案先改本文件，再同步 Windows 安装器；macOS 安装包（DMG 必读文件 / pkg 欢迎页 / App 首启提示）均直接取用本文件，避免多语言文案漂移。
- 同步到 `.iss` 时，段落分隔使用 `%n%n`。

## 完整版（安装器首屏使用）

### 中文（zh）

**欢迎使用 OpenTune 安装程序**

本软件永久开源免费。如果您付费购买了此软件，请联系购买平台退款。唯一官方发布渠道为 GitHub 页面（github.com/YuFeng926/OpenTune）。

售卖 OpenVPI 声码器权重（包括随本软件一起分发的模型文件）、或以收取安装费、服务费、会员费等任何名义变相收费的行为，均属违反 CC BY-NC-SA 4.0 许可。任何违反 AGPLv3 协议而分发此软件的行为将构成著作权侵权。DAYA STUDIO 与 OpenVPI Team 将依法追究其法律责任。

### English (en)

**Welcome to OpenTune Setup**

This software is permanently free and open source. If you paid to purchase this software, please contact the purchase platform for a refund. The only official release channel is the GitHub page (github.com/YuFeng926/OpenTune).

Selling the OpenVPI vocoder weights (including the model files distributed together with this software), or charging any disguised fees (such as installation fees, service fees, or membership fees) for them, violates the CC BY-NC-SA 4.0 license. Distributing this software in violation of the AGPLv3 license constitutes copyright infringement. DAYA STUDIO and the OpenVPI Team will pursue legal remedies against such violations.

### 日本語（ja）

**OpenTune セットアップへようこそ**

本ソフトウェアは永久に無料のオープンソースです。本ソフトウェアを有料で購入された場合は、購入元のプラットフォームに返金をご請求ください。唯一の公式配布チャンネルは GitHub ページ（github.com/YuFeng926/OpenTune）です。

OpenVPI のボコーダー重み（本ソフトウェアと共に配布されるモデルファイルを含む）の販売、またはインストール費・サービス費・会員費等の名目によるいかなる変則的な課金も、CC BY-NC-SA 4.0 ライセンスに違反します。AGPLv3 ライセンスに違反して本ソフトウェアを配布する行為は著作権侵害を構成します。DAYA STUDIO および OpenVPI Team は法的措置を講じます。

### Русский（ru）

**Добро пожаловать в установку OpenTune**

Эта программа навсегда бесплатна и распространяется как открытое ПО. Если вы заплатили за её приобретение, обратитесь на площадку покупки для возврата средств. Единственный официальный канал распространения — страница GitHub (github.com/YuFeng926/OpenTune).

Продажа весов вокодера OpenVPI (включая файлы моделей, распространяемые вместе с этим программным обеспечением) или взимание любых скрытых платежей (таких как плата за установку, обслуживание или членство) нарушает лицензию CC BY-NC-SA 4.0. Распространение этого программного обеспечения с нарушением лицензии AGPLv3 является нарушением авторских прав. DAYA STUDIO и OpenVPI Team примут юридические меры.

### Español (es)

**Bienvenido al instalador de OpenTune**

Este software es gratuito y de código abierto de forma permanente. Si pagó por adquirirlo, solicite un reembolso a la plataforma de compra. El único canal oficial de distribución es la página de GitHub (github.com/YuFeng926/OpenTune).

Vender los pesos del vocoder de OpenVPI (incluidos los archivos de modelo distribuidos junto con este software), o cobrar cualquier tarifa encubierta (como tarifas de instalación, servicio o membresía) por ellos, infringe la licencia CC BY-NC-SA 4.0. Distribuir este software en violación de la licencia AGPLv3 constituye una infracción de derechos de autor. DAYA STUDIO y OpenVPI Team emprenderán acciones legales.

## 简版（DMG 背景等窄空间场景，中文 / 英文）

### 中文

OpenTune 永久开源免费。付费购买请向购买平台申请退款；唯一官方渠道为 GitHub 页面（github.com/YuFeng926/OpenTune）。售卖随包声码器权重、或以安装费/服务费等名义变相收费，违反 CC BY-NC-SA 4.0 与 AGPLv3，将依法追责。

### English

OpenTune is permanently free and open source. If you paid for it, request a refund from the platform of purchase; the only official channel is the GitHub page (github.com/YuFeng926/OpenTune). Selling the bundled OpenVPI vocoder weights or charging disguised fees violates CC BY-NC-SA 4.0 and AGPLv3.

## macOS 落点建议（供实现参考）

1. **DMG 背景 / 内置「安装前必读」文件**：无需证书，挂载即见；背景用简版，必读文件放完整版。
2. **`.pkg` 欢迎页**（`productbuild` Distribution 的 Welcome/ReadMe 页面）：与 Windows 安装器体验一致，需考虑签名与公证。
3. **App 首次启动提示**：可复用 Onboarding 设施，覆盖 VST3 与便携版用户，作为前两者的补充。
