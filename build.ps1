$env:MSYSTEM=$null
. "C:\Espressif\tools\Microsoft.v6.0.3.PowerShell_profile.ps1"
cd 'C:\Users\koral\Documents\EspZero-v2'
idf.py set-target esp32 2>&1 | Out-File -Encoding utf8 C:\Users\koral\Documents\EspZero-v2\build.log
idf.py build 2>&1 | Out-File -Append -Encoding utf8 C:\Users\koral\Documents\EspZero-v2\build.log
"EXITCODE=$LASTEXITCODE" | Out-File -Append -Encoding utf8 C:\Users\koral\Documents\EspZero-v2\build.log
