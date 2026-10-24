# ⚠️ WARNING ⚠️
This driver was made with the help of generative AI. Use at your own risk.

# geekbook-fan-driver
A Linux kernel module to control the fans of the GeekBook M16

**Installation**
```
sudo mkdir /usr/src/geekbook_ec-0.1/
sudo cp geekbook_ec.c Makefile dkms.conf /usr/src/geekbook_ec-0.1/
sudo dkms install geekbook_ec/0.1
sudo modprobe geekbook_ec
echo geekbook_ec | sudo tee /etc/modules-load.d/geekbook_ec.conf
sudo systemctl restart systemd-modules-load
```

**Uninstall**
```
sudo rmmod geekbook_ec
sudo rm /etc/modules-load.d/geekbook_ec.conf
sudo dkms remove geekbook_ec/0.1 --all
sudo rm -rf /usr/src/geekbook_ec-0.1
```

