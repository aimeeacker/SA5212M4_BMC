#!/usr/bin/env node
/**
 * repack.mjs - 浪潮 SA5212M4 BMC 固件一键编译、打包与校验全流程引擎 (纯 Node.js 24 实现)
 *
 * 功能流程:
 * 1. 检查构建环境 (gcc, arm-linux-gnueabi-gcc, fakeroot, mkfs.cramfs, 7z)
 * 2. 自动解压 7z 基础固件包 (若未就绪)
 * 3. 编译主机加解密工具 (tea_iroot)
 * 4. 校验并准备 rootfs 客制化组件 (fan_control.conf, ld.so.preload, passwd, shadow)
 * 5. 交叉编译 AST2300 ARMv5TE 软浮点劫持库 (libfanhook.so) 并校验
 * 6. 在 fakeroot 环境下还原 109 个 Linux 核心特殊设备节点并构建 CramFS 镜像
 * 7. 调用 tea_iroot 添加 uImage 头并执行 128-bit TEA 硬件加密
 * 8. 写入 32MB SPI Flash 镜像 (保留原厂 4.35.0 版本定义)
 * 9. 自动计算并修复 8 个 FMH 头部 8-bit Checksum 及整包 IEEE 802.3 CRC32
 * 10. 纯 Node.js 构建标准 UEFI Shell 极速刷写包 (uefi_flash_pack.zip)
 */

import fs from "node:fs";
import path from "node:path";
import cp from "node:child_process";
import crypto from "node:crypto";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";
import { extractCramfs, parseInode } from "./uncramfs.mjs";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const REPO_ROOT = path.resolve(__dirname, "..");
const SRC_DIR = path.resolve(REPO_ROOT, "src");
const ROOTFS_DIR = path.resolve(REPO_ROOT, "rootfs");
const UEFI_DIR = path.resolve(REPO_ROOT, "uefi_flash_pack");

const BASE_ROM_PATH = path.resolve(REPO_ROOT, "SA5212M4_BMC_4.35.0_Standard_20191025");
const BASE_ROM_7Z = path.resolve(REPO_ROOT, "SA5212M4_BMC_4.35.0_Standard_20191025.7z");
const OUTPUT_ROM_PATH = path.resolve(REPO_ROOT, "SA5212M4_BMC_4.35.0_Standard_Custom.bin");
const OUTPUT_ZIP_PATH = path.resolve(REPO_ROOT, "uefi_flash_pack.zip");
const TEA_TOOL = path.resolve(SRC_DIR, "tea_iroot");

const IROOT_OFFSET = 0x150000;
const IROOT_SIZE = 0xFEE040;        // 64 字节 uImage 头 + 16,703,488 字节 CramFS
const AST_FMH_OFFSET = 0x01FF0000;  // ast2300e FMH 偏移
const EXPECTED_ROM_SIZE = 32 * 1024 * 1024;
const FMH_SIZE = 64;
const FMH_END_MAGIC = 0x55AA;

// 109 个 Linux 特殊设备节点定义 [name, type, major, minor, mode]
const DEV_NODES = [["adc0","c",251,0,511],["cipher_dev","c",126,0,511],["console","c",5,1,511],["full","c",1,7,511],["gpio0","c",101,0,511],["i2c-0","c",89,0,511],["i2c-1","c",89,1,511],["i2c-2","c",89,2,511],["i2c-3","c",89,3,511],["i2c-4","c",89,4,511],["i2c-5","c",89,5,511],["i2c-6","c",89,6,511],["i2c-7","c",89,7,511],["i2c-8","c",89,8,511],["i2c-9","c",89,9,511],["i2c0","c",89,0,511],["i2c1","c",89,1,511],["i2c2","c",89,2,511],["i2c3","c",89,3,511],["i2c4","c",89,4,511],["i2c5","c",89,5,511],["i2c6","c",89,6,511],["i2c7","c",89,7,511],["i2c8","c",89,8,511],["i2c9","c",89,9,511],["initctl","p",0,0,511],["ipauth","c",96,3,511],["ipl","c",95,0,511],["ipnat","c",95,1,511],["ipstate","c",95,2,511],["kcs0","c",42,0,511],["kcs1","c",42,1,511],["kcs2","c",42,2,511],["kmem","c",1,2,511],["loop0","b",7,0,511],["loop1","b",7,1,511],["loop2","b",7,2,511],["loop3","b",7,3,511],["lpcuart0","c",103,0,511],["mem","c",1,1,511],["mmcblk0","b",179,0,511],["mmcblk0p1","b",179,1,511],["mmcblk0p2","b",179,2,511],["mmcblk0p3","b",179,3,511],["mmcblk1","b",179,8,511],["mmcblk1p1","b",179,9,511],["mmcblk1p2","b",179,10,511],["mmcblk1p3","b",179,11,511],["mtd0","c",90,0,511],["mtd1","c",90,2,511],["mtd10","c",90,20,511],["mtd2","c",90,4,511],["mtd3","c",90,6,511],["mtd4","c",90,8,511],["mtd5","c",90,10,511],["mtd6","c",90,12,511],["mtd7","c",90,14,511],["mtd8","c",90,16,511],["mtd9","c",90,18,511],["mtdblock1","b",31,1,511],["mtdblock2","b",31,2,511],["mtdblock3","b",31,3,511],["mtdblock4","b",31,4,511],["mtdblock5","b",31,5,511],["mtdblock6","b",31,6,511],["mtdblock7","b",31,7,511],["mtdblock8","b",31,8,511],["mtdr0","c",90,1,511],["mtdr1","c",90,3,511],["mtdr2","c",90,5,511],["mtdr3","c",90,7,511],["mtdr4","c",90,9,511],["mtdr5","c",90,11,511],["mtdr6","c",90,13,511],["mtdr7","c",90,15,511],["mtdr8","c",90,17,511],["netmon","c",125,0,511],["null","c",1,3,511],["nvram","c",10,144,511],["peci0","c",45,0,511],["port","c",1,4,511],["ptmx","c",5,2,511],["pwmtach0","c",46,0,511],["ram0","b",1,0,511],["ram1","b",1,1,511],["ram2","b",1,2,511],["ram3","b",1,3,511],["ram4","b",1,4,511],["random","c",1,8,511],["reset","c",110,0,511],["rtc","c",10,135,511],["snoop0","c",44,0,511],["socksys","c",30,0,511],["spx","c",30,1,511],["tty","c",5,0,511],["ttyC0","c",204,8,511],["ttyC1","c",204,9,511],["ttyC2","c",204,10,511],["ttyS0","c",4,64,511],["ttyS1","c",4,65,511],["ttyS2","c",4,66,511],["ttyS3","c",4,67,511],["ttyS4","c",4,68,511],["ttyS5","c",4,69,511],["urandom","c",1,9,511],["usb","c",100,0,511],["videocap","c",15,0,511],["watchdog","c",10,130,511],["zero","c",1,5,511]];

function logStep(step, total, title) {
    console.log(`\n\x1b[36m[${step}/${total}]\x1b[0m \x1b[1m${title}\x1b[0m`);
}

function logSuccess(msg) {
    console.log(`  \x1b[32m[✓]\x1b[0m ${msg}`);
}

function logInfo(msg) {
    console.log(`  \x1b[34m[*]\x1b[0m ${msg}`);
}

function logError(msg) {
    console.error(`  \x1b[31m[-] 错误: ${msg}\x1b[0m`);
}

function runCmd(cmd, args, opts = {}) {
    logInfo(`执行命令: ${cmd} ${args.join(" ")}`);
    const res = cp.spawnSync(cmd, args, {
        cwd: opts.cwd || REPO_ROOT,
        stdio: opts.silent ? "pipe" : "inherit",
        env: { ...process.env, ...opts.env },
    });
    if (res.status !== 0) {
        logError(`命令失败 (退出码 ${res.status}): ${cmd} ${args.join(" ")}`);
        if (res.stderr) console.error(res.stderr.toString());
        process.exit(res.status || 1);
    }
    return res;
}

function checkTool(toolName, aptPackage) {
    const res = cp.spawnSync("which", [toolName], { stdio: "pipe" });
    if (res.status !== 0) {
        logError(`缺少必要工具: ${toolName} (请安装: sudo apt-get install -y ${aptPackage || toolName})`);
        process.exit(1);
    }
}

/**
 * 遍历 CramFS 镜像二进制数据，验证特殊设备节点数量
 */
function verifyCramfsNodes(cramfsImgPath) {
    const data = fs.readFileSync(cramfsImgPath);
    let count = 0;

    function scanDir(dirOffset, dirSize) {
        let pos = dirOffset;
        const end = dirOffset + dirSize;
        while (pos < end) {
            if (pos + 12 > data.length) break;
            const inode = parseInode(data, pos);
            pos += 12;
            if (inode.namelen === 0) break;
            pos += inode.namelen;
            const ftype = inode.mode & 0o170000;
            if (ftype !== 0o040000 && ftype !== 0o100000 && ftype !== 0o120000) {
                count++;
            }
            if (ftype === 0o040000 && inode.size > 0 && inode.offset > 0) {
                scanDir(inode.offset, inode.size);
            }
        }
    }

    const root = parseInode(data, 64);
    scanDir(root.offset, root.size);
    return count;
}

/**
 * 校验并自动修复 32MB SPI Flash 镜像中全部 FMH 8-bit Checksum 及整包 IEEE 802.3 CRC32
 */
export function fixImageChecksums(inputFile, outputFile = inputFile) {
    if (!fs.existsSync(inputFile)) {
        logError(`输入文件不存在: ${inputFile}`);
        return false;
    }

    const data = fs.readFileSync(inputFile);
    let fixedFmhCount = 0;
    let pos = 0;
    const needle = Buffer.from("$MODULE$");

    // 1. 扫描并修复所有合法的 FMH (Flash Module Header) 8-bit Checksum
    while (true) {
        const idx = data.indexOf(needle, pos);
        if (idx === -1) break;
        if (idx + FMH_SIZE <= data.length) {
            const endMagic = data.readUInt16LE(idx + 0x3E);
            if (endMagic === FMH_END_MAGIC) {
                const modNameRaw = data.subarray(idx + 0x18, idx + 0x20);
                const nullIdx = modNameRaw.indexOf(0);
                const modName = (nullIdx !== -1 ? modNameRaw.subarray(0, nullIdx) : modNameRaw).toString("latin1");

                data[idx + 0x17] = 0;
                let sum = 0;
                for (let i = 0; i < FMH_SIZE; i++) sum += data[idx + i];
                const chk8 = (-sum) & 0xFF;
                data[idx + 0x17] = chk8;
                logInfo(`校验/修复 FMH @ 0x${idx.toString(16).padStart(8, "0").toUpperCase()} [${modName.padEnd(8, " ")}] -> 8-bit Checksum: 0x${chk8.toString(16).padStart(2, "0").toUpperCase()}`);
                fixedFmhCount++;
            }
        }
        pos = idx + 8;
    }

    logInfo(`共校验/修复了 ${fixedFmhCount} 个 FMH 头部`);

    // 2. 计算并回填整包 IEEE 802.3 CRC32 到 ast2300e FMH 头部
    if (data.length >= AST_FMH_OFFSET + FMH_SIZE) {
        const stream = Buffer.concat([
            data.subarray(0, AST_FMH_OFFSET + 0x17),
            data.subarray(AST_FMH_OFFSET + 0x18, AST_FMH_OFFSET + 0x32),
            data.subarray(AST_FMH_OFFSET + 0x36)
        ]);
        const calcCrc = zlib.crc32(stream) >>> 0;
        logInfo(`计算整包 CRC32 (已剔除 5 字节自身位): 0x${calcCrc.toString(16).padStart(8, "0").toUpperCase()}`);
        data.writeUInt32LE(calcCrc, AST_FMH_OFFSET + 0x32);

        data[AST_FMH_OFFSET + 0x17] = 0;
        let sum = 0;
        for (let i = 0; i < FMH_SIZE; i++) sum += data[AST_FMH_OFFSET + i];
        const astChk8 = (-sum) & 0xFF;
        data[AST_FMH_OFFSET + 0x17] = astChk8;
        logInfo(`刷新 ast2300e FMH 最终 8-bit Checksum: 0x${astChk8.toString(16).padStart(2, "0").toUpperCase()}`);
    }

    fs.writeFileSync(outputFile, data);
    logSuccess(`固件校验和修复完成，已保存至: ${outputFile}`);
    return true;
}

/**
 * 纯 Node.js 24 实现的标准 ZIP 格式打包器 (无须系统 zip 命令与 python zipfile)
 */
export function createZipFromDir(srcDir, outputZipPath) {
    const files = [];
    const entries = fs.readdirSync(srcDir, { withFileTypes: true });

    for (const ent of entries) {
        const fullPath = path.join(srcDir, ent.name);
        if (ent.isFile()) {
            files.push({
                name: ent.name,
                data: fs.readFileSync(fullPath),
            });
        }
    }

    files.sort((a, b) => a.name.localeCompare(b.name));

    const localHeaders = [];
    const centralEntries = [];
    let offset = 0;

    for (const f of files) {
        const nameBuf = Buffer.from(f.name, "utf8");
        const uncompressedSize = f.data.length;
        const crc = zlib.crc32(f.data) >>> 0;
        const compressed = zlib.deflateRawSync(f.data);
        const compressedSize = compressed.length;

        // Local file header (30 字节 + 文件名)
        const lh = Buffer.alloc(30 + nameBuf.length);
        lh.writeUInt32LE(0x04034b50, 0);       // local file header signature
        lh.writeUInt16LE(20, 4);               // version needed to extract (2.0)
        lh.writeUInt16LE(0, 6);                // general purpose bit flag
        lh.writeUInt16LE(8, 8);                // compression method (deflate)
        lh.writeUInt16LE(0, 10);               // last mod file time
        lh.writeUInt16LE(0, 12);               // last mod file date
        lh.writeUInt32LE(crc, 14);             // crc-32
        lh.writeUInt32LE(compressedSize, 18);   // compressed size
        lh.writeUInt32LE(uncompressedSize, 22); // uncompressed size
        lh.writeUInt16LE(nameBuf.length, 26);  // file name length
        lh.writeUInt16LE(0, 28);               // extra field length
        nameBuf.copy(lh, 30);

        localHeaders.push(lh, compressed);

        // Central directory file header (46 字节 + 文件名)
        const ch = Buffer.alloc(46 + nameBuf.length);
        ch.writeUInt32LE(0x02014b50, 0);       // central file header signature
        ch.writeUInt16LE(20, 4);               // version made by
        ch.writeUInt16LE(20, 6);               // version needed to extract
        ch.writeUInt16LE(0, 8);                // general purpose bit flag
        ch.writeUInt16LE(8, 10);               // compression method (deflate)
        ch.writeUInt16LE(0, 12);               // last mod file time
        ch.writeUInt16LE(0, 14);               // last mod file date
        ch.writeUInt32LE(crc, 16);             // crc-32
        ch.writeUInt32LE(compressedSize, 20);   // compressed size
        ch.writeUInt32LE(uncompressedSize, 24); // uncompressed size
        ch.writeUInt16LE(nameBuf.length, 28);  // file name length
        ch.writeUInt16LE(0, 30);               // extra field length
        ch.writeUInt16LE(0, 32);               // file comment length
        ch.writeUInt16LE(0, 34);               // disk number start
        ch.writeUInt16LE(0, 36);               // internal file attributes
        ch.writeUInt32LE((0o100644 << 16) >>> 0, 38); // external file attributes
        ch.writeUInt32LE(offset, 42);          // relative offset of local header
        nameBuf.copy(ch, 46);

        centralEntries.push(ch);
        offset += lh.length + compressed.length;
    }

    const cdOffset = offset;
    const cdSize = centralEntries.reduce((acc, c) => acc + c.length, 0);

    // End of central directory record (22 字节)
    const eocd = Buffer.alloc(22);
    eocd.writeUInt32LE(0x06054b50, 0);         // end of central dir signature
    eocd.writeUInt16LE(0, 4);                  // number of this disk
    eocd.writeUInt16LE(0, 6);                  // number of the disk with the start of central dir
    eocd.writeUInt16LE(files.length, 8);       // total number of entries on this disk
    eocd.writeUInt16LE(files.length, 10);      // total number of entries in central dir
    eocd.writeUInt32LE(cdSize, 12);            // size of central directory
    eocd.writeUInt32LE(cdOffset, 16);          // offset of start of central directory
    eocd.writeUInt16LE(0, 20);                 // .ZIP file comment length

    const zipBuffer = Buffer.concat([...localHeaders, ...centralEntries, eocd]);
    fs.writeFileSync(outputZipPath, zipBuffer);
    logSuccess(`纯 Node.js 标准 ZIP 打包完成: ${outputZipPath} (${zipBuffer.length} 字节, ${files.length} 个文件)`);
}

/**
 * 在 fakeroot 环境下还原 109 个设备节点并调用 mkfs.cramfs
 */
function buildCramfsUnderFakeroot(rootfsDir, outputCramfs) {
    const devDir = path.join(rootfsDir, "dev");
    fs.mkdirSync(devDir, { recursive: true });

    console.log(`[*] 正在 fakeroot 环境下还原 109 个设备节点至 ${devDir}...`);
    for (const [name, type, maj, min, mode] of DEV_NODES) {
        const devPath = path.join(devDir, name);
        try { fs.unlinkSync(devPath); } catch {}
        if (type === "p") {
            const res = cp.spawnSync("mkfifo", [devPath], { stdio: "inherit" });
            if (res.status !== 0) {
                console.error(`[-] 无法创建管道节点: ${devPath}`);
                process.exit(1);
            }
        } else {
            const res = cp.spawnSync("mknod", [devPath, type, String(maj), String(min)], { stdio: "inherit" });
            if (res.status !== 0) {
                console.error(`[-] 无法创建设备节点: ${devPath}`);
                process.exit(1);
            }
        }
        cp.spawnSync("chmod", [mode.toString(8), devPath]);
    }

    console.log(`[*] 正在调用 mkfs.cramfs 构建 CramFS 镜像...`);
    const mkRes = cp.spawnSync("mkfs.cramfs", [
        "-b", "4096",
        "-e", "0",
        "-N", "little",
        "-n", "Compressed ROMFS",
        rootfsDir,
        outputCramfs,
    ], { stdio: "inherit" });

    if (mkRes.status !== 0) {
        console.error(`[-] mkfs.cramfs 执行失败 (退出码: ${mkRes.status})`);
        process.exit(mkRes.status || 1);
    }
    console.log(`[✓] CramFS 镜像构建完成: ${outputCramfs}`);
}

function verifyRootfs(rootfsDir) {
    logInfo(`检查 rootfs 关键配置: ${rootfsDir}`);
    const checks = [
        ["usr/local/lib/libfanhook.so", "自动风扇控制劫持库"],
        ["etc/ld.so.preload", "动态链接预加载配置"],
        ["etc/fan_control.conf", "风扇调速主配置文件"],
        ["etc/defconfig/shadow", "默认管理员密码配置 (admin)"],
        ["etc/passwd", "系统账户 shell 配置"],
    ];
    for (const [relPath, desc] of checks) {
        const fullPath = path.join(rootfsDir, relPath);
        if (!fs.existsSync(fullPath)) {
            logError(`缺少关键文件 [${desc}]: ${fullPath}`);
            process.exit(1);
        }
        logSuccess(`${desc}: ${relPath}`);
    }

    // 验证 ld.so.preload
    const preload = fs.readFileSync(path.join(rootfsDir, "etc/ld.so.preload"), "utf8");
    if (!preload.includes("/usr/local/lib/libfanhook.so")) {
        logError("etc/ld.so.preload 中未包含 /usr/local/lib/libfanhook.so!");
        process.exit(1);
    }

    // 验证 passwd
    const passwd = fs.readFileSync(path.join(rootfsDir, "etc/passwd"), "utf8");
    for (const user of ["root", "sysadmin"]) {
        const matched = passwd.split("\n").some((l) => l.startsWith(`${user}:`) && (l.endsWith("/bin/sh") || l.endsWith("/bin/bash")));
        if (!matched) {
            logError(`账户 ${user} 的登录 shell 异常!`);
            process.exit(1);
        }
    }
}

async function main() {
    console.log("\x1b[35m" + "=".repeat(66));
    console.log("  浪潮 SA5212M4 BMC 固件构建引擎 (Node.js " + process.version + ")");
    console.log("  目标架构: ASPEED AST2300 (ARMv5TE, soft-float, glibc 2.11)");
    console.log("=".repeat(66) + "\x1b[0m");

    // 1. 检查构建工具依赖
    logStep(1, 8, "检查系统构建工具依赖");
    checkTool("gcc", "build-essential");
    checkTool("arm-linux-gnueabi-gcc", "gcc-arm-linux-gnueabi");
    checkTool("fakeroot", "fakeroot");
    checkTool("mkfs.cramfs", "util-linux");
    logSuccess("系统核心编译构建工具检查通过 (纯 Node.js 工具链，无需 Python/Zip)");

    // 2. 检查并解压基准 32MB 固件
    logStep(2, 8, "验证基准固件 (SA5212M4_BMC_4.35.0_Standard_20191025)");
    if (!fs.existsSync(BASE_ROM_PATH)) {
        if (fs.existsSync(BASE_ROM_7Z)) {
            logInfo("检测到 7z 固件压缩包，正在自动解压基准固件...");
            checkTool("7z", "p7zip-full");
            runCmd("7z", ["x", "-y", BASE_ROM_7Z], { cwd: REPO_ROOT });
            if (!fs.existsSync(BASE_ROM_PATH)) {
                logError("解压后未找到基准固件文件!");
                process.exit(1);
            }
            logSuccess("基准固件解压完成");
        } else {
            logError(`找不到基准固件: ${BASE_ROM_PATH}\n  提示: 请将浪潮原厂 32MB 固件放置于仓库根目录 (SA5212M4_BMC_4.35.0_Standard_20191025) 后再执行构建。`);
            process.exit(1);
        }
    }
    const baseStat = fs.statSync(BASE_ROM_PATH);
    if (baseStat.size !== EXPECTED_ROM_SIZE) {
        logError(`基准固件大小异常: ${baseStat.size} 字节 (应为 33,554,432 字节)`);
        process.exit(1);
    }
    logSuccess(`基准固件大小校验通过 (32MB, ${baseStat.size} 字节)`);

    // 3. 编译主机工具 (tea_iroot)
    logStep(3, 8, "编译主机加解密工具 (tea_iroot)");
    runCmd("make", ["-C", SRC_DIR, "tea_iroot"], { cwd: REPO_ROOT });
    logSuccess("主机加解密工具生成成功");

    // 4. 校验并准备 rootfs 客制化组件
    logStep(4, 8, "校验并准备 rootfs 客制化组件");
    if (!fs.existsSync(ROOTFS_DIR)) {
        logInfo("rootfs 目录未就绪，正在从基准固件中解密并解包还原...");
        const tmpIrootDec = "/tmp/iroot_dec_build.bin";
        runCmd(TEA_TOOL, ["decrypt", BASE_ROM_PATH, tmpIrootDec]);
        extractCramfs(tmpIrootDec, ROOTFS_DIR);
        if (fs.existsSync(tmpIrootDec)) fs.unlinkSync(tmpIrootDec);
    }

    // 同步 fan_control.conf
    const confSrc = path.join(SRC_DIR, "fan_control.conf");
    const confDst1 = path.join(ROOTFS_DIR, "etc/fan_control.conf");
    const confDst2 = path.join(ROOTFS_DIR, "etc/defconfig/fan_control.conf");
    fs.copyFileSync(confSrc, confDst1);
    if (fs.existsSync(path.dirname(confDst2))) {
        fs.copyFileSync(confSrc, confDst2);
    }
    logSuccess("同步 /etc/fan_control.conf 与 /etc/defconfig/fan_control.conf");

    // 校验 ld.so.preload 包含 /usr/local/lib/libfanhook.so
    const preloadPath = path.join(ROOTFS_DIR, "etc/ld.so.preload");
    let preloadContent = fs.existsSync(preloadPath) ? fs.readFileSync(preloadPath, "utf8") : "";
    if (!preloadContent.includes("/usr/local/lib/libfanhook.so")) {
        preloadContent += "\n/usr/local/lib/libfanhook.so\n";
        fs.writeFileSync(preloadPath, preloadContent);
        logInfo("已向 /etc/ld.so.preload 写入 libfanhook.so");
    }
    logSuccess("/etc/ld.so.preload 预加载声明已确认");

    // 配置 root:admin / sysadmin:admin 账户与标准 /bin/sh Shell
    const passwdPath = path.join(ROOTFS_DIR, "etc/passwd");
    fs.writeFileSync(passwdPath, "root:x:0:0:root:/root:/bin/sh\nsysadmin:x:0:0:sysadmin:/root:/bin/sh\n");
    logSuccess("配置 /etc/passwd 启用 root/sysadmin 标准 /bin/sh Shell");

    const shadowDef = path.join(ROOTFS_DIR, "etc/defconfig/shadow");
    if (fs.existsSync(path.dirname(shadowDef))) {
        fs.writeFileSync(shadowDef, "root:$1$A17c6z5w$fjBLueH75zrBTl8Ujoylu1:2:0:99999:7:::\nsysadmin:$1$A17c6z5w$fjBLueH75zrBTl8Ujoylu1:2:0:99999:7:::\n");
        logSuccess("配置 /etc/defconfig/shadow 赋予 root/sysadmin 初始默认密码 (admin)");
    }

    // 5. 交叉编译 AST2300 智能温控劫持库 (libfanhook.so)
    logStep(5, 8, "交叉编译 AST2300 智能调速劫持库 (libfanhook.so)");
    runCmd("make", ["-C", SRC_DIR, "libfanhook.so", "verify"], { cwd: REPO_ROOT });
    logSuccess("libfanhook.so 编译与兼容性校验通过 (ARMv5TE, GLIBC_2.4, SYSV hash)");

    // 同步 libfanhook.so 到 rootfs
    const hookSrc = path.join(SRC_DIR, "libfanhook.so");
    const hookDst = path.join(ROOTFS_DIR, "usr/local/lib/libfanhook.so");
    fs.copyFileSync(hookSrc, hookDst);
    logSuccess("同步最新编译的 /usr/local/lib/libfanhook.so");

    verifyRootfs(ROOTFS_DIR);

    // 6. 执行固件完整重打包 (CramFS, uImage, TEA, Version 4.35.0, FMH Checksums, CRC32)
    logStep(6, 8, "构建 CramFS 并生成加密 iroot Payload (保留原厂 4.35.0)");
    const tmpCramfs = "/tmp/cramfs_repack.img";
    const tmpIrootEnc = "/tmp/iroot_encrypted.bin";

    // 使用 fakeroot 还原 109 个设备节点并打包 CramFS
    logInfo("调用 fakeroot 生成 CramFS 镜像...");
    runCmd("fakeroot", ["node", __filename, "--build-cramfs", ROOTFS_DIR, tmpCramfs]);

    const cramfsSz = fs.statSync(tmpCramfs).size;
    logSuccess(`CramFS 镜像生成成功，大小: ${cramfsSz} 字节 (0x${cramfsSz.toString(16).toUpperCase()})`);
    if (cramfsSz > 16703488) {
        logError(`CramFS 大小 ${cramfsSz} 超过分区上限 16,703,488 字节 (0xFEE000)!`);
        process.exit(1);
    }

    const devCount = verifyCramfsNodes(tmpCramfs);
    logSuccess(`CramFS 特殊设备节点校验通过: 共 ${devCount} 个节点 (含 /dev/console, /dev/kcs0, /dev/pwmtach0 等)`);
    if (devCount < 100) {
        logError(`CramFS 设备节点缺失 (${devCount} < 100)，终止打包!`);
        process.exit(1);
    }

    // 调用 tea_iroot 添加 uImage 头并执行 TEA 硬件加密
    logInfo("生成 uImage 头并执行 128-bit TEA 硬件加密...");
    runCmd(TEA_TOOL, ["encrypt", tmpCramfs, tmpIrootEnc]);

    const encSz = fs.statSync(tmpIrootEnc).size;
    if (encSz !== IROOT_SIZE) {
        logError(`加密后大小为 ${encSz} 字节，预期为 ${IROOT_SIZE} 字节!`);
        process.exit(1);
    }
    logSuccess(`加密 Payload 生成成功 (${encSz} 字节，精确对齐)`);

    // 读取基准 ROM，注入加密 iroot payload
    logInfo(`写入基准 32MB SPI Flash: ${BASE_ROM_PATH} -> ${OUTPUT_ROM_PATH}`);
    const romData = fs.readFileSync(BASE_ROM_PATH);
    const encPayload = fs.readFileSync(tmpIrootEnc);
    encPayload.copy(romData, IROOT_OFFSET);
    fs.writeFileSync(OUTPUT_ROM_PATH, romData);
    logSuccess(`成功注入加密 iroot payload 到 0x${IROOT_OFFSET.toString(16).toUpperCase()} - 0x${(IROOT_OFFSET + IROOT_SIZE).toString(16).toUpperCase()}`);

    // 修复全部 FMH 8-bit Checksum 及整包 IEEE 802.3 CRC32
    logInfo("修复 8 个 FMH 头部校验和及 ast2300e CRC32...");
    if (!fixImageChecksums(OUTPUT_ROM_PATH, OUTPUT_ROM_PATH)) {
        logError("校验和修复失败!");
        process.exit(1);
    }

    // 7. 导出 UEFI Shell 离线刷写组件与纯 Node.js ZIP 打包
    logStep(7, 8, "同步 UEFI Shell 刷写包与生成 ZIP 归档");
    fs.mkdirSync(UEFI_DIR, { recursive: true });

    // 导出 iroot_payload.bin (用于快速单分区刷写)
    fs.copyFileSync(tmpIrootEnc, path.join(UEFI_DIR, "iroot_payload.bin"));
    fs.copyFileSync(tmpIrootEnc, path.join(REPO_ROOT, "iroot_payload.bin"));

    // 导出全量 32MB ROM (用于全量恢复刷写)
    fs.copyFileSync(OUTPUT_ROM_PATH, path.join(UEFI_DIR, path.basename(OUTPUT_ROM_PATH)));

    // 清理已废弃的旧版本或冗余文件
    const oldFiles = [
        path.join(UEFI_DIR, "version_fmh.bin"),
        path.join(UEFI_DIR, "SA5212M4_BMC_4.35.1_Standard_Custom.bin"),
    ];
    for (const of of oldFiles) {
        if (fs.existsSync(of)) fs.unlinkSync(of);
    }

    logSuccess("已同步 iroot_payload.bin 与 32MB Custom ROM 到 uefi_flash_pack/");

    // 纯 Node.js 打包 uefi_flash_pack.zip
    createZipFromDir(UEFI_DIR, OUTPUT_ZIP_PATH);

    // 清理临时文件
    if (fs.existsSync(tmpCramfs)) fs.unlinkSync(tmpCramfs);
    if (fs.existsSync(tmpIrootEnc)) fs.unlinkSync(tmpIrootEnc);

    // 8. 最终哈希计算与汇总报告
    logStep(8, 8, "计算发布产物 SHA-256 校验和");
    const romBuf = fs.readFileSync(OUTPUT_ROM_PATH);
    const romSha256 = crypto.createHash("sha256").update(romBuf).digest("hex");

    const zipBuf = fs.readFileSync(OUTPUT_ZIP_PATH);
    const zipSha256 = crypto.createHash("sha256").update(zipBuf).digest("hex");

    console.log("\n\x1b[32m" + "=".repeat(66));
    console.log("  [✓] 固件重编译与自动化打包全部完成！(全链路 Node.js 24)");
    console.log("=".repeat(66) + "\x1b[0m");
    console.log(`\n  \x1b[1m32MB 完整 ROM 镜像:\x1b[0m`);
    console.log(`    文件路径: ${OUTPUT_ROM_PATH}`);
    console.log(`    文件大小: ${romBuf.length} 字节 (32.00 MB)`);
    console.log(`    固件版本: 4.35.0 (Standard Custom, 保留原厂版本定义)`);
    console.log(`    SHA-256 : ${romSha256}`);
    console.log(`\n  \x1b[1mUEFI Shell 离线刷写包 (纯 Node.js 生成):\x1b[0m`);
    console.log(`    文件路径: ${OUTPUT_ZIP_PATH}`);
    console.log(`    文件大小: ${zipBuf.length} 字节 (${(zipBuf.length / 1024 / 1024).toFixed(2)} MB)`);
    console.log(`    SHA-256 : ${zipSha256}`);
    console.log("\n" + "=".repeat(66) + "\n");
}

// 命令行参数分流处理
if (process.argv.includes("--build-cramfs")) {
    const rootfsIdx = process.argv.indexOf("--build-cramfs") + 1;
    const rootfs = process.argv[rootfsIdx] || ROOTFS_DIR;
    const outImg = process.argv[rootfsIdx + 1] || "/tmp/cramfs_repack.img";
    buildCramfsUnderFakeroot(rootfs, outImg);
    process.exit(0);
}

if (process.argv.includes("--fix-checksums")) {
    const romFile = process.argv[process.argv.indexOf("--fix-checksums") + 1] || OUTPUT_ROM_PATH;
    fixImageChecksums(romFile, romFile);
    process.exit(0);
}

if (process.argv.includes("--zip")) {
    const idx = process.argv.indexOf("--zip");
    const sDir = process.argv[idx + 1] || UEFI_DIR;
    const oZip = process.argv[idx + 2] || OUTPUT_ZIP_PATH;
    createZipFromDir(sDir, oZip);
    process.exit(0);
}

main().catch((err) => {
    logError(err.stack || err.message);
    process.exit(1);
});
