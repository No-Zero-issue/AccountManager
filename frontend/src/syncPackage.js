// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// syncPackage.js —— 「同步到手机」的同步包格式（纯 JS，不依赖 Vue，便于单独验证）
//
// 一页同步包 = 一段纯文本，行之间用 \n 分隔：
//
//   AM2FA/1                                    ← 魔数 + 格式版本
//   1/3                                        ← 本页序号 / 总页数
//   名称 \t 类型 \t 密钥 \t 算法 \t 位数 \t 周期 \t 撤销码   ← 一条记录一行，制表符分隔
//
// 字段说明：
//   name        名称（名称里的制表符 / 换行在打包前已换成空格）
//   type        steam = Steam Guard（5 位字母码）；totp = 标准 TOTP
//   secret      Base32 密钥（Steam 的 Base64 shared_secret 在桌面端先转成 Base32）
//   algorithm   SHA1 / SHA256 / SHA512
//   digits      6 / 8 …（Steam 固定 5）
//   period      30 或自定义秒数
//   revocation  撤销码，可空（只有 Steam 条目才有）
//
// 手机端对应实现：android/app/src/main/java/com/accountmanager/steamguard/SyncPackage.kt
// 两边格式必须一致；改动格式时请同时改 version 号与两边的单测。

export const SYNC_MAGIC = 'AM2FA/1';

/**
 * 单页字节上限。二维码是「版本越高格子越密」，越大越难扫；
 * 900 字节大约对应二维码 version 30 左右，在 440px 下每个格子还有 3 像素以上。
 */
export const SYNC_PAGE_BYTE_LIMIT = 900;

/** 每页开头两行（魔数 + 页码）要占掉的字节数，分页时先预留出来 */
const PAGE_HEADER_RESERVE = 24;

const utf8Length = (text) => {
  let bytes = 0;
  for (const ch of text) {
    const code = ch.codePointAt(0);
    if (code < 0x80) bytes += 1;
    else if (code < 0x800) bytes += 2;
    else if (code < 0x10000) bytes += 3;
    else bytes += 4;
  }
  return bytes;
};

/** 把一条记录拼成一行（制表符分隔） */
export const syncRecordLine = (record) => [
  record.name,
  record.type,
  record.secret,
  record.algorithm,
  String(record.digits),
  String(record.period),
  record.revocation || ''
].join('\t');

/**
 * 把记录按字节预算分页，返回每一页的完整文本。
 * @param {Array<{name:string,type:string,secret:string,algorithm:string,digits:number,period:number,revocation?:string}>} records
 * @param {number} pageByteLimit
 * @returns {{pages: string[], count: number}}
 */
export const buildSyncPages = (records, pageByteLimit = SYNC_PAGE_BYTE_LIMIT) => {
  const chunks = [];
  let current = [];
  let currentBytes = 0;
  const lineBudget = Math.max(1, pageByteLimit - PAGE_HEADER_RESERVE);

  for (const record of records) {
    const line = syncRecordLine(record);
    const lineBytes = utf8Length(line) + 1;   // +1 = 换行
    if (current.length > 0 && currentBytes + lineBytes > lineBudget) {
      chunks.push(current);
      current = [];
      currentBytes = 0;
    }
    current.push(line);
    currentBytes += lineBytes;
  }
  chunks.push(current);   // 至少一页，保证调用方总能拿到 pages[0]

  const total = chunks.length;
  const pages = chunks.map((lines, index) => [SYNC_MAGIC, `${index + 1}/${total}`, ...lines].join('\n'));
  return { pages, count: records.length };
};
