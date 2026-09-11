@echo -off
echo =======================================================
echo Flashing SA5212M4 BMC RootFS (16MB)...
echo =======================================================
socflash.efi if=iroot_payload.bin offset=0x150000 count=0xFEE040
echo =======================================================
echo Flash completed successfully!
echo Please power cycle or reset BMC to apply changes!
echo =======================================================
