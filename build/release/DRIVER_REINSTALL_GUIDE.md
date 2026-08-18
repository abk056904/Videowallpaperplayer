# NVIDIA Driver Reinstall Guide (DDU + Fresh Install)

**Goal:** Register the NVDEC hardware decoder MFT with Media Foundation.

**Pre-downloaded:**
- NVIDIA driver 610.88: `build/release/driver/nvidia_610.88.exe` (934 MB)
- DDU: Download from https://www.wagnardsoft.com/display-driver-uninstaller-ddu (v18.1.5.6)

---

## Step 1: Create System Restore Point

1. Press `Win + S`, type "Create a restore point", open it
2. Click "Create..." → name it "Before DDU driver reinstall" → Create
3. Wait for confirmation

## Step 2: Download DDU

1. Go to https://www.wagnardsoft.com/display-driver-uninstaller-ddu
2. Click "Download DDU" → save the zip
3. Extract to `C:\DDU` (or anywhere)

## Step 3: Boot into Safe Mode

1. Press `Win + R`, type `msconfig`, press Enter
2. Go to "Boot" tab
3. Check "Safe boot" → select "Minimal"
4. Click OK → Restart
5. The machine will boot into Safe Mode (low resolution, basic display)

## Step 4: Run DDU

1. In Safe Mode, navigate to `C:\DDU` (or wherever you extracted it)
2. Run `DDU v18.1.5.6.exe` (or `Display Driver Uninstaller.exe`)
3. Select "GPU" → "NVIDIA"
4. Click "Clean and restart"
5. DDU will remove ALL NVIDIA and AMD drivers, then restart

## Step 5: Install New NVIDIA Driver

1. After restart (normal mode, basic display), navigate to:
   `C:\Users\mbk43\Desktop\Videowallpaperplayer\build\release\driver\nvidia_610.88.exe`
2. Run the installer
3. Choose "Custom" → check "Perform a clean installation"
4. Complete the installation
5. Restart when prompted

## Step 6: Verify

1. After restart, open a terminal and run:
   ```
   nvidia-smi --query-gpu=name,driver_version --format=csv,noheader
   ```
   Expected: `NVIDIA GeForce RTX 3050 Laptop GPU, 610.88`

2. Run the VideoWallpaper app with debug logging:
   ```
   # Set logLevel to debug in config.json
   # Run the app
   # Check the log for "hardware decode active" instead of "hardware decode unavailable"
   ```

3. If HW decode works:
   - CPU should drop from ~136% to <10%
   - RAM should drop from ~444 MB to ~250 MB
   - Log should show `hardwareDecode=yes`

---

## Troubleshooting

**If display doesn't work after DDU:**
- The machine is using Microsoft Basic Display Adapter (normal)
- Just install the NVIDIA driver — it will restore full display

**If driver install fails:**
- Download the driver again from nvidia.com/drivers
- Try the "Express" installation instead of "Custom"

**If HW decode still doesn't work after driver install:**
- The issue is deeper (MF stack, Windows update, or registry)
- Fall back to P2-P6 software optimizations
