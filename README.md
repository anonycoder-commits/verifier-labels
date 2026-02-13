# Verifier Labels

<p align="center">
  <img src="logo.png" width="160" alt="Verifier Labels Logo">
</p>

## Features

* **Verifier Credits**: See the name of the player(s) who verified the level
* **Video Proof**: Clickable YouTube icon that opens the verification video in your browser
* **Two-Player Support**: Toggle between Solo and 2-Player verifier info for dual-mode levels
* **Platformer Support**: Works with both classic and platformer Extreme Demons
* **Smart Caching**: Data is saved locally to ensure instant loading and reduced API calls
* **Legacy Indicators**: Optionally display legacy list levels in a grayed-out style
* **Customizable**: Adjust label position, alignment, and feature visibility via Geode settings

---

## How to Use

1. Open any **Extreme Demon** level (5 demon stars or higher)
2. The verifier's name appears automatically below the level creator info
3. For **two-player levels**, click the label to toggle between Solo and 2P verifier data
4. Click the **YouTube icon** to watch the verification video (when available)

### Status Messages
* **"Verified by: [Name]"** - Level is on the AREDL
* **"[Solo] Verified by: [Name]"** - Solo verification for a two-player level
* **"[2P] [Names]"** - Two-player verification
* **"Not on AREDL"** - Level is not listed in the database
* **"Checking..."** - Currently fetching data from the API

---

## Configuration

Access these settings in the **Geode mod menu**:

| Setting | Description |
|---------|-------------|
| **Show Label** | Toggle the mod UI on or off |
| **Y-Offset** | Move the label up or down to avoid overlapping other mods |
| **Label Alignment** | Choose between Left or Center alignment |
| **Show YouTube Button** | Toggle the visibility of the video link icon |
| **Gray Out Legacy** | Display legacy list levels in gray text |
| **Disable Cache** | Turn off local caching (not recommended) |

---

## Technical Details

* **API Source**: [AREDL API v2](https://api.aredl.net)
* **Cache File**: `verifier_cache.json` (stored in mod save directory)
* **Cache Expiry**: 30 minutes for "Not on AREDL" entries
* **Supported Modes**: Classic Demons & Platformer Demons

---

## Credits

* **Mod Author**: Tasuposed
* **Data Source**: [AREDL](https://aredl.net/) - All Rated Extreme Demons List
