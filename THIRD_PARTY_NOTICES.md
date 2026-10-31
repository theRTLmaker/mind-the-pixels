# Third-party notices

This project is distributed under the MIT License. The following third-party
software or assets retain their own licenses.

## TRMNL16 Regular

The embedded ASCII bitmap table in
`components/zectrix_demo_ui/font/zectrix_ascii_font_8x16.h` was rasterized from
TRMNL16 Regular by Heavyweight Digital Type Foundry. The font is licensed under
the SIL Open Font License, Version 1.1, included at
`licenses/TRMNL_FONT_LICENSE.txt`.

## Espressif ESP-IDF

ESP-IDF is provided by Espressif Systems under its respective open-source
licenses. It is a build dependency and is not copied into this project.

## Espressif esp_codec_dev

The audio codec abstraction is obtained through ESP-IDF Component Manager as
`espressif/esp_codec_dev`. Its license file is delivered with the downloaded
component in `managed_components/espressif__esp_codec_dev/LICENSE`.

## Demonstration artwork

The upstream lighthouse, snowy-path and mountain demo images (MIT, Zectrix
Lab) are no longer included; they remain in the git history and upstream.

## Data sources

These services are queried at run time; none of their data is stored in or
distributed with this repository.

- Weather: [Open-Meteo](https://open-meteo.com/), licensed under
  CC BY 4.0, which requires attribution.
- Trains: National Rail Darwin, through the
  [Rail Data Marketplace](https://raildata.org.uk/) Live Departure Board –
  Staff Version (subject to that product's terms and your own API key) or
  the public [Huxley2](https://github.com/jpsingleton/Huxley2) proxy.
