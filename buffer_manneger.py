import sys
import socket
import threading
import time
import queue
from PyQt6.QtWidgets import QMainWindow, QApplication, QMessageBox
from PyQt6.QtCore import QTimer, QMutex, QByteArray, pyqtSignal, QObject
import pyaudio
import numpy as np

class AudioBuffer(QObject):
    update_signal = pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.input_queue = queue.Queue()
        self.output_queue = queue.Queue()
        self.buffer_size = 3  # Default buffer size in frames
        self.mutex = QMutex()
        self.is_running = True
        self.adjust_thread = threading.Thread(target=self.adjust_buffer)
        self.adjust_thread.start()

    def set_buffer_size(self, size):
        with self.mutex:
            self.buffer_size = size
            self.update_signal.emit(f"Buffer size set to {size} frames")

    def adjust_buffer(self):
        while self.is_running:
            time.sleep(1)  # Check every second
            with self.mutex:
                current_size = self.output_queue.qsize()
                if current_size < self.buffer_size and not self.input_queue.empty():
                    # Need to fill buffer
                    while not self.input_queue.empty() and self.output_queue.qsize() < self.buffer_size:
                        self.output_queue.put(self.input_queue.get())
                elif current_size > self.buffer_size * 1.5:
                    # Need to reduce buffer
                    while self.output_queue.qsize() > self.buffer_size:
                        try:
                            self.output_queue.get_nowait()
                        except queue.Empty:
                            break

    def add_input(self, data):
        with self.mutex:
            self.input_queue.put(data)

    def get_output(self):
        with self.mutex:
            if not self.output_queue.empty():
                return self.output_queue.get()
            return None

    def stop(self):
        self.is_running = False
        self.adjust_thread.join()


class ChatWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.audio_buffer = AudioBuffer()
        self.audio_buffer.update_signal.connect(self.update_status)
        self.init_ui()
        self.init_audio()

    def init_ui(self):
        # ... (ваш существующий UI код)

        # Добавляем элементы управления буфером
        self.buffer_size_spinbox.valueChanged.connect(self.set_buffer_size)

    def set_buffer_size(self, size):
        self.audio_buffer.set_buffer_size(size)

    def init_audio(self):
        self.audio = pyaudio.PyAudio()
        self.format = pyaudio.paInt16
        self.channels = 1
        self.rate = 48000
        self.chunk = int(self.rate * 0.04)  # 40ms chunks

        # Input stream
        self.input_stream = self.audio.open(
            format=self.format,
            channels=self.channels,
            rate=self.rate,
            input=True,
            frames_per_buffer=self.chunk,
            stream_callback=self.input_callback
        )

        # Output stream
        self.output_stream = self.audio.open(
            format=self.format,
            channels=self.channels,
            rate=self.rate,
            output=True,
            frames_per_buffer=self.chunk,
            stream_callback=self.output_callback
        )

    def input_callback(self, in_data, frame_count, time_info, status):
        # Добавляем данные во входную очередь буфера
        self.audio_buffer.add_input(in_data)
        return (None, pyaudio.paContinue)

    def output_callback(self, out_data, frame_count, time_info, status):
        # Получаем данные из выходной очереди буфера
        data = self.audio_buffer.get_output()
        if data is not None:
            return (data, pyaudio.paContinue)
        # Если данных нет, возвращаем тишину
        silence = np.zeros(self.chunk, dtype=np.int16).tobytes()
        return (silence, pyaudio.paContinue)

    def update_status(self, message):
        self.debug_area.append(message)

    def closeEvent(self, event):
        self.audio_buffer.stop()
        self.input_stream.stop_stream()
        self.output_stream.stop_stream()
        self.input_stream.close()
        self.output_stream.close()
        self.audio.terminate()
        super().closeEvent(event)


if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = ChatWindow()
    window.show()
    sys.exit(app.exec())
