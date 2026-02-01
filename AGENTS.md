# JoystickTesting

The purpose of this project is to send commands to a UniFi Protect camera system
to control PTZ via a Joystick attached to a Windows computer.

Some things the program will need to do:
- Use Windows-native networking systems to connect to HTTPS endpoints using
  Schannel so the system trusted roots are used.
- Connect to UniFi software, use a username and password to sign in with a POST
  request.
  - For reference, there is an example showing how to log in in the file
    [Unifi_Client.cs](references/Unifi_Client.cs).
- Cache the response, containing CSRF and login tokens, re-using them on each
  subsequent request.
- Open up Windows input management to retrieve joystick state
- Handle dead zone mapping
- Map the joystick input to a POST request to control a specific camera's
  movement.

The UniFi system roughly expects the movement POST requests to contain these
fields:

POST to:
https://192.168.3.251/proxy/protect/api/cameras/67a2bce203a1b203e4001891/move

```json
{
  "type":"continuous",
  "payload":{
    "x":0,"y":0,"z":0
  }
}
```

Simply map the X, Y, and Z values from the Joystick to the coordinates in the
request, and send it out periodically. The interval is yet to be decided.

For testing that builds are successful, this command should work:
```
cmake --build .\out\build\x64-debug\ --target JoystickTesting
```
