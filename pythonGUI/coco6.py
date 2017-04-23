import sys

from PyQt5.QtCore import *
from PyQt5.QtWidgets import *
from PyQt5.QtGui import *
import qdarkstyle

import base64
import io
import select
import socket

import subprocess

class WaveWin(QWidget):
	def __init__(self):
		super().__init__()
	
		self.isconnected = False
		
		lay = QVBoxLayout()
		lay.setContentsMargins(0, 0, 0, 0)
		lay.setSpacing(0)
		
		self.topwid = QWidget()
		self.topwid.setFixedSize(QSize(200, 750))
		
		self.toplay = QVBoxLayout()
		self.toplay.setContentsMargins(0, 0, 0, 0)
		self.toplay.setSpacing(0)
		self.topwid.setLayout(self.toplay)
		self.toplay.setAlignment(Qt.AlignCenter | Qt.AlignTop)
		self.topwid.setStyleSheet("QWidget { background-color: black; color: #ff0; font: 12pt 'Asap', sans-serif; }")

		self.wavedisplay = []
		for i in range(30):
			self.wavedisplay.append(QLabel())
		
		for i in range(30):
			self.wavedisplay[i].setFixedSize(QSize(200, 25))
			self.toplay.addWidget(self.wavedisplay[i])
		
		self.button = QPushButton("Connect", self)
		self.button.clicked.connect(self.goconnect)
		
		lay.addWidget(self.button)
		lay.addWidget(self.topwid)
		self.setLayout(lay)
		
		self.setFixedSize(QSize(200, 780))

	@pyqtSlot(str, str)
	def wave_out(self, id, jpeg):
		i = int(id)
		if i >= 0 and i < 30:
			xim = QImage.fromData(base64.b64decode(jpeg.encode()))
			self.wavedisplay[i].setPixmap(QPixmap.fromImage(xim))
	
	@pyqtSlot()
	def goconnect(self):
		if self.isconnected == True:
			return
		
		text, ok = QInputDialog.getText(self, "Input Name", "Your Name:")
		if ok:
			if len(text) > 1 and len(text) < 30:
				name = text
			else:
				return
		else:
			return
		
		text, ok = QInputDialog.getText(self, "Input Room", "Join a Room:")
		if ok:
			if len(text) > 1 and len(text) < 30:
				room = text
			else:
				return
		else:
			return
		
		self.process = subprocess.Popen(["coco6_loop.exe", base64.b64encode(name.encode()).decode("utf-8"), base64.b64encode(room.encode()).decode("utf-8")], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
		
		self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
		try:
			self.sock.connect(("188.68.38.124", 9100))
			logintxt = "tclient " + base64.b64encode(name.encode()).decode("utf-8") + " " + base64.b64encode(room.encode()).decode("utf-8") + "\n"
			self.sock.send(logintxt.encode())
			self.isconnected = True
			self.inbuf = b""
			
			self.workloop = Work(self)
			self.athread = QThread()
			self.workloop.moveToThread(self.athread)
			self.workloop.waveout.connect(self.wave_out)
			self.athread.started.connect(self.workloop.work)
			self.athread.start()
		except:
			pass

class Work(QObject):
	waveout = pyqtSignal(str, str)
	
	def __init__(self, parent):
		super().__init__()
		self.p = parent
	
	@pyqtSlot()
	def work(self):
		global app
		while self.p.isconnected:
			app.processEvents()

			r, w, e = select.select([self.p.sock], [], [], 0.1)
			
			if not r:
				continue
			
			rc = None
			try:
				rc = self.p.sock.recv(1)
			except:
				pass
			
			if not rc:
				self.p.sock.close()
				self.p.isconnected = False
				break

			if rc == b"\n":
				txt = self.p.inbuf.decode("utf-8").split(" ")
				if txt[0] == "jpeg" and len(txt) == 3:
					id = txt[1]
					jpeg = txt[2]
					self.waveout.emit(id, jpeg)
				
				self.p.inbuf = b""
	
			else:
				self.p.inbuf += rc

class MainWin(QMainWindow):
	def __init__(self):
		super().__init__()
		
		self.setGeometry(100, 100, 200, 732)
		self.setWindowTitle("Coco6")
		self.setWindowIcon(QIcon("chat-icon.png"))
		
		qr = self.frameGeometry()
		cp = QDesktopWidget().availableGeometry().center()
		qr.moveCenter(cp)
		self.move(qr.topLeft())
		
		self.wave_window = WaveWin()
		self.setCentralWidget(self.wave_window)
		
		self.setFixedSize(self.wave_window.size())
		
		self.show()

global app
app = QApplication(sys.argv)
app.setStyleSheet(qdarkstyle.load_stylesheet_pyqt5())
main_window = MainWin()
app.exec_()
main_window.wave_window.isconnected = False
main_window.wave_window.process.kill()
