from socket import socket, AF_INET, SOCK_STREAM

def echo_client(address):
    s = socket(AF_INET, SOCK_STREAM)
    s.connect((address, 20000))

    s.send(b'Hello\n')
    resp = s.recv(8192)
    print('Response:', resp)
    s.close()

if __name__ == '__main__':
    print "starting echo_server"
    echo_client('10.163.58.69')