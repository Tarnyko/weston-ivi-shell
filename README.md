Weston IVI-Shell
===============

<strong>A GENIVI-oriented shell interface for the Weston compositor

contact : TANIBATA Nabuhiko (https://github.com/ntanibata)</strong>


## Description

 <strong>IVI-Shell</strong> provides a shell interface for Weston, which maps the GENIVI API (http://www.genivi.org) for In-Vehicle Infotainment.

 This repository provides the upstream Weston, with the IVI-Shell patches integrated in the master branch or most common release branches. It will be updated frequently. It may be helpful to test the most recent IVI-Shell patches with the most recent Weston.

![IVI-Shell screenshots](http://lists.freedesktop.org/archives/wayland-devel/attachments/20140625/abbfc064/attachment-0001.png)

## Installation

* Compile Weston normally.

  Then, after having built and installed Weston, still at the root folder, install the ivi-shell-specific "weston.ini" configuration file like this :
<strong>cp ivi-shell/weston.ini $HOME/.config/weston.ini</strong>
  Run Weston.
