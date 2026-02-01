GET `/v1/cameras`

Response sample:

```json
[
  {
    "id": "string",
    "modelKey": "string",
    "state": "CONNECTED",
    "name": "string",
    "mac": "string",
    "isMicEnabled": true,
    "osdSettings": {
      "isNameEnabled": true,
      "isDateEnabled": true,
      "isLogoEnabled": true,
      "isDebugEnabled": true,
      "overlayLocation": "topLeft"
    },
    "ledSettings": {
      "isEnabled": true,
      "welcomeLed": true,
      "floodLed": true
    },
    "lcdMessage": {
      "type": "LEAVE_PACKAGE_AT_DOOR",
      "resetAt": null,
      "text": "string"
    },
    "micVolume": 0,
    "activePatrolSlot": 0,
    "videoMode": "default",
    "hdrType": "auto",
    "featureFlags": {
      "supportFullHdSnapshot": true,
      "hasHdr": true,
      "smartDetectTypes": [
        "person"
      ],
      "smartDetectAudioTypes": [
        "alrmSmoke"
      ],
      "videoModes": [
        "default"
      ],
      "hasMic": true,
      "hasLedStatus": true,
      "hasSpeaker": true
    },
    "smartDetectSettings": {
      "objectTypes": [
        "person"
      ],
      "audioTypes": [
        "alrmSmoke"
      ]
    }
  }
]
```