# m5stick-totp
TOTP Authenticator for M5StickCPlus2

This is a basic standalone TOTP authenticator for the M5StickCPlus2. You can export your codes from the google auth app, translate them with the included local html file, and upload them to a captive portal web page hosted on the M5StickCPlus2. It also pairs as a BLE keyboard, so pressing the A button sends the keystrokes for the auth code to the host device. A long press of A also sends a return/enter keystroke at the end. PWR and B buttons are used to cycle up and down the list of codes. A long press of C will enter Setup mode where you can log in to the web portal and change wifi settings and add/remove auth codes. It uses ntp for accurate time, so you need to configure a wifi network to connect to on boot for that.

You are responsible for your own security decisions regarding the use of this code.

Required Libraries (Arduino IDE):
 * - M5StickCPlus2
 * - TOTP-Arduino by lucadentella

- Compile and install from Arduino IDE
- Start device and connect to WiFi network: M5Stick-TOTP (If needed, you can force it into setup mode by holding the B button)
- Captive portal page is available at 192.168.4.1
- Set your wifi SSID and password there
- Export your auth codes from google authenticator, convert them using included google-auth-decoder.html, and paste the results into the web portal
- Restart the M5StickCPlus2 and pair with it as a bluetooth keyboard

<img width="724" height="454" alt="m5stick-totp" src="https://github.com/user-attachments/assets/24bb5b27-047e-42fd-b8d5-083bb2723ce9" />
