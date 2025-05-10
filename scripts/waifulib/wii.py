# encoding: utf-8
# wii.py --  DOL task
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.


#This module is used as a blank, when the compiler finishes linking xash3d.elf, use 'elf2dol' to make it into a executable for the homebrew channel
#If you're planning to implement this look into the switch port, it has a similar functionality
from waflib.Tools import ccroot
from waflib import *

