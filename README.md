# Verifier Labels

A Geode mod that displays verifier information and video links for Extreme Demon levels in Geometry Dash.

<img src="logo.png" width="150" alt="the mod's logo" />

## Features

- **Verifier Display**: Shows the name of the person who verified Extreme Demon levels
- **Video Links**: Provides clickable YouTube icons that open verification videos
- **Smart Caching**: Locally caches verifier data to reduce API calls and improve performance
- **AREDL Integration**: Uses the AREDL (Archive of Extreme Demon Layouts) API for accurate verification data
- **Thread-Safe**: Implements proper concurrency handling for cache operations

## How It Works

The mod automatically fetches verifier information for Extreme Demon levels using the AREDL API. The data is cached locally to minimize network requests and provide instant results on subsequent views.

## Installation

1. Install [Geode SDK](https://docs.geode-sdk.org/getting-started/)
2. Build the mod using the instructions below
3. Place the compiled `.geode` file in your Geode mods folder

## Build Instructions

For more information, see [the Geode documentation](https://docs.geode-sdk.org/getting-started/create-mod#build)

```sh
# Assuming you have the Geode CLI set up already
geode build
```

## Configuration

The mod automatically manages its cache file in your mod's save directory. No manual configuration is required.

## Requirements

- Geometry Dash 2.2081 or later
- Geode SDK 5.0.0-alpha.1 or later
- geode.node-ids dependency (>= 1.22.0-beta.1)

## Resources

- [Geode SDK Documentation](https://docs.geode-sdk.org/)
- [Geode SDK Source Code](https://github.com/geode-sdk/geode/)
- [Geode CLI](https://github.com/geode-sdk/cli)
- [Bindings](https://github.com/geode-sdk/bindings/)
- [Dev Tools](https://github.com/geode-sdk/DevTools)
- [AREDL API](https://aredl.net/)
