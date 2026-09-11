#!/usr/bin/env node
/**
 * uncramfs.mjs - 纯 Node.js 24 实现的 CramFS 镜像解析与提取工具
 * 支持解析 CramFS 超级块、目录树遍历、zlib 数据解压、文件权限与符号链接恢复。
 */

import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

export function parseInode(data, pos) {
    const mode = data.readUInt16LE(pos);
    const uid = data.readUInt16LE(pos + 2);
    const b2 = data.readUInt32LE(pos + 4);
    const b3 = data.readUInt32LE(pos + 8);
    const sz = b2 & 0x00FFFFFF;
    const gid = (b2 >> 24) & 0xFF;
    const namelen = (b3 & 0x3F) << 2;
    const offset = (b3 >> 6) & 0x03FFFFFF;
    return { mode, uid, size: sz, gid, namelen, offset: offset << 2 };
}

export function extractCramfs(imgPath, outDir) {
    if (!fs.existsSync(imgPath)) {
        throw new Error(`[-] 错误: 找不到 CramFS 镜像: ${imgPath}`);
    }

    const data = fs.readFileSync(imgPath);
    const magic = data.readUInt32LE(0);
    if (magic !== 0x28cd3d45) {
        throw new Error(`[-] 错误: 非法 CramFS 魔数: 0x${magic.toString(16)} (预期 0x28cd3d45)`);
    }

    const size = data.readUInt32LE(4);
    console.log(`[+] CramFS 魔数校验通过: 0x${magic.toString(16)}, 大小: ${size} 字节`);

    const root = parseInode(data, 64);
    fs.mkdirSync(outDir, { recursive: true });

let fileCount = 0;
let dirCount = 0;
let linkCount = 0;
let devCount = 0;

function extractDir(dirOffset, dirSize, currentPath) {
    let pos = dirOffset;
    const end = dirOffset + dirSize;
    while (pos < end) {
        if (pos + 12 > data.length) break;
        const inode = parseInode(pos);
        pos += 12;
        if (inode.namelen === 0) break;
        const nameRaw = data.subarray(pos, pos + inode.namelen);
        pos += inode.namelen;
        const nullIdx = nameRaw.indexOf(0);
        const name = (nullIdx !== -1 ? nameRaw.subarray(0, nullIdx) : nameRaw).toString("latin1");
        if (!name) continue;

        const fullPath = path.join(currentPath, name);
        const ftype = inode.mode & 0o170000;

        if (ftype === 0o040000) {
            dirCount++;
            fs.mkdirSync(fullPath, { recursive: true });
            if (inode.size > 0 && inode.offset > 0) {
                extractDir(inode.offset, inode.size, fullPath);
            }
            try { fs.chmodSync(fullPath, inode.mode & 0o777); } catch {}
        } else if (ftype === 0o100000) {
            fileCount++;
            const nblocks = Math.floor((inode.size + 4095) / 4096);
            const blockPtrs = [];
            for (let i = 0; i < nblocks; i++) {
                blockPtrs.push(data.readUInt32LE(inode.offset + i * 4));
            }
            const chunks = [];
            let startPtr = inode.offset + nblocks * 4;
            for (let i = 0; i < nblocks; i++) {
                const endPtr = blockPtrs[i];
                const chunk = data.subarray(startPtr, endPtr);
                startPtr = endPtr;
                if (chunk.length > 0) {
                    try {
                        chunks.push(zlib.inflateSync(chunk));
                    } catch {
                        chunks.push(chunk);
                    }
                }
            }
            const fileData = Buffer.concat(chunks).subarray(0, inode.size);
            fs.writeFileSync(fullPath, fileData);
            try { fs.chmodSync(fullPath, inode.mode & 0o777); } catch {}
        } else if (ftype === 0o120000) {
            linkCount++;
            const nblocks = Math.floor((inode.size + 4095) / 4096);
            const blockPtrs = [];
            for (let i = 0; i < nblocks; i++) {
                blockPtrs.push(data.readUInt32LE(inode.offset + i * 4));
            }
            const chunks = [];
            let startPtr = inode.offset + nblocks * 4;
            for (let i = 0; i < nblocks; i++) {
                const endPtr = blockPtrs[i];
                const chunk = data.subarray(startPtr, endPtr);
                startPtr = endPtr;
                if (chunk.length > 0) {
                    try {
                        chunks.push(zlib.inflateSync(chunk));
                    } catch {
                        chunks.push(chunk);
                    }
                }
            }
            const target = Buffer.concat(chunks).subarray(0, inode.size).toString("latin1");
            try { fs.unlinkSync(fullPath); } catch {}
            try { fs.symlinkSync(target, fullPath); } catch {}
        } else {
            devCount++;
        }
    }
}

    extractDir(root.offset, root.size, outDir);
    console.log(`[✓] CramFS 提取完成: ${dirCount} 个目录, ${fileCount} 个文件, ${linkCount} 个符号链接, ${devCount} 个特殊设备节点。`);
    return { dirCount, fileCount, linkCount, devCount };
}

// CLI direct run check
if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(import.meta.filename)) {
    const img = process.argv[2] || "cramfs.img";
    const out = process.argv[3] || "rootfs";
    try {
        extractCramfs(img, out);
    } catch (err) {
        console.error(err.message);
        process.exit(1);
    }
}
