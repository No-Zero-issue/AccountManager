// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// otpauth.js —— 构造 otpauth:// 链接（Google Authenticator Key URI 格式）
//   纯 JS、不依赖 Vue，便于单独验证；App.vue 负责把条目里的密钥归一化成下面的字段。
//
// 入参：
//   type         'steam' | 'totp'
//   label        显示名（条目名）
//   account      账号名，可空（标准 TOTP 会拼成 Label:账号）
//   secretBase32 Base32 密钥（Steam 的 Base64 shared_secret 由调用方先转成 Base32）
//   algorithm    'SHA1' | 'SHA256' | 'SHA512'
//   digits       6 / 8 …（Steam 固定 5）
//   period       30 或自定义秒数
//
// 两个约定：
//   1) Steam 走 `otpauth://totp/Steam:<名字>?secret=<Base32>&issuer=Steam`
//      —— 手机端靠 `Steam:` / `issuer=Steam` 认出这是 Steam Guard（5 位字母码）；
//   2) 标准 TOTP 只有非默认值才写进查询串（SHA1 / 6 位 / 30 秒不写），
//      链接更短，Google Authenticator / Aegis 等任意验证器都认。

export const buildOtpAuthUri = ({
  type = 'totp',
  label = '',
  account = '',
  secretBase32 = '',
  algorithm = 'SHA1',
  digits = 6,
  period = 30
} = {}) => {
  const secret = String(secretBase32 || '').trim();
  if (!secret) return '';

  const name = String(label || account || '账号').trim() || '账号';
  const params = new URLSearchParams();

  if (type === 'steam') {
    params.set('secret', secret);
    params.set('issuer', 'Steam');
    return `otpauth://totp/Steam:${encodeURIComponent(name)}?${params.toString()}`;
  }

  params.set('secret', secret);
  params.set('issuer', name);
  if (algorithm && algorithm !== 'SHA1') params.set('algorithm', algorithm);
  if (Number(digits) !== 6) params.set('digits', String(digits));
  if (Number(period) !== 30) params.set('period', String(period));

  const userName = String(account || '').trim();
  const path = userName && userName !== name ? `${name}:${userName}` : name;
  return `otpauth://totp/${encodeURIComponent(path)}?${params.toString()}`;
};
