// Maryati Bridge host proxy: HTTP/HTTPS CONNECT on :8080 and SOCKS5 on :1080.
// Standard library only, so it builds statically and runs without admin rights.
package main

import (
	"bufio"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"
)

const (
	httpProxyAddr  = ":8080"
	socksProxyAddr = ":1080"
	bridgeUDPAddr  = "192.168.33.1:33333"
	bridgeSubnet   = "192.168.33.0/24"
	dialTimeout    = 15 * time.Second
)

var hopHeaders = []string{
	"Connection",
	"Proxy-Connection",
	"Keep-Alive",
	"Proxy-Authenticate",
	"Proxy-Authorization",
	"Te",
	"Trailer",
	"Transfer-Encoding",
	"Upgrade",
}

// Direct transport: never follow HTTP_PROXY from the environment, or the
// request could be sent back to this very proxy.
var directTransport = func() *http.Transport {
	tr := http.DefaultTransport.(*http.Transport).Clone()
	tr.Proxy = nil
	return tr
}()

func main() {
	log.SetFlags(log.Ltime)

	lanIP := detectLANIP()
	if lanIP == nil {
		log.Printf("warning: no LAN IPv4 found, ESP32 will not learn the host IP")
	} else {
		log.Printf("host LAN IP: %s", lanIP)
		notifyBridge(lanIP)
	}

	errCh := make(chan error, 2)
	go func() { errCh <- runHTTPProxy() }()
	go func() { errCh <- runSOCKS5Proxy() }()
	if err := <-errCh; err != nil {
		log.Fatalf("proxy stopped: %v", err)
	}
}

// detectLANIP prefers the source address of the default route, then falls back
// to the first usable private IPv4 on a physical-looking interface.
func detectLANIP() net.IP {
	if conn, err := net.Dial("udp4", "8.8.8.8:53"); err == nil {
		ip := conn.LocalAddr().(*net.UDPAddr).IP
		conn.Close()
		if isLANIP(ip) {
			return ip
		}
	}

	ifaces, err := net.Interfaces()
	if err != nil {
		return nil
	}
	var fallback net.IP
	for _, ifi := range ifaces {
		if ifi.Flags&net.FlagUp == 0 || ifi.Flags&net.FlagLoopback != 0 || isVirtualIface(ifi.Name) {
			continue
		}
		addrs, err := ifi.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			ip = ip.To4()
			if ip == nil || !isLANIP(ip) {
				continue
			}
			if ip.IsPrivate() {
				return ip
			}
			if fallback == nil {
				fallback = ip
			}
		}
	}
	return fallback
}

func isLANIP(ip net.IP) bool {
	ip4 := ip.To4()
	if ip4 == nil || ip4.IsLoopback() || ip4.IsUnspecified() || ip4.IsLinkLocalUnicast() {
		return false
	}
	_, bridge, _ := net.ParseCIDR(bridgeSubnet)
	return !bridge.Contains(ip4)
}

func isVirtualIface(name string) bool {
	for _, prefix := range []string{"docker", "veth", "br-", "virbr", "tun", "tap"} {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	return false
}

// notifyBridge tells the OLED which host LAN IP to display. UDP is fire and
// forget; a missing ESP32 only means no status update.
func notifyBridge(ip net.IP) {
	conn, err := net.Dial("udp4", bridgeUDPAddr)
	if err != nil {
		log.Printf("UDP notify failed: %v", err)
		return
	}
	defer conn.Close()

	_ = conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	msg := "CMD:PROXY_ON:" + ip.String()
	if _, err := conn.Write([]byte(msg)); err != nil {
		log.Printf("UDP notify failed: %v", err)
		return
	}
	log.Printf("notified ESP32: %s", msg)
}

func runHTTPProxy() error {
	server := &http.Server{
		Addr:              httpProxyAddr,
		Handler:           http.HandlerFunc(handleHTTP),
		ReadHeaderTimeout: 15 * time.Second,
	}
	log.Printf("HTTP/HTTPS CONNECT proxy listening on %s", httpProxyAddr)
	return server.ListenAndServe()
}

func handleHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodConnect {
		handleConnect(w, r)
		return
	}
	handleForward(w, r)
}

func handleConnect(w http.ResponseWriter, r *http.Request) {
	target := r.Host
	if _, _, err := net.SplitHostPort(target); err != nil {
		target = net.JoinHostPort(target, "443")
	}

	upstream, err := net.DialTimeout("tcp", target, dialTimeout)
	if err != nil {
		http.Error(w, "upstream dial failed", http.StatusBadGateway)
		return
	}
	defer upstream.Close()

	hijacker, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "hijacking not supported", http.StatusInternalServerError)
		return
	}
	client, buffered, err := hijacker.Hijack()
	if err != nil {
		return
	}
	defer client.Close()

	if _, err := client.Write([]byte("HTTP/1.1 200 Connection Established\r\n\r\n")); err != nil {
		return
	}

	// buffered.Reader still holds any bytes the client sent early.
	go func() {
		_, _ = io.Copy(upstream, buffered.Reader)
		closeWrite(upstream)
	}()
	_, _ = io.Copy(client, upstream)
}

func handleForward(w http.ResponseWriter, r *http.Request) {
	if !r.URL.IsAbs() {
		http.Error(w, "proxy requires an absolute-form request URI", http.StatusBadRequest)
		return
	}

	out := r.Clone(r.Context())
	out.RequestURI = ""
	removeHopHeaders(out.Header)

	resp, err := directTransport.RoundTrip(out)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	removeHopHeaders(resp.Header)
	for key, values := range resp.Header {
		for _, value := range values {
			w.Header().Add(key, value)
		}
	}
	w.WriteHeader(resp.StatusCode)
	_, _ = io.Copy(w, resp.Body)
}

func removeHopHeaders(header http.Header) {
	// Drop headers named by Connection first, before Connection itself goes.
	for _, value := range header.Values("Connection") {
		for _, name := range strings.Split(value, ",") {
			if name = strings.TrimSpace(name); name != "" {
				header.Del(name)
			}
		}
	}
	for _, name := range hopHeaders {
		header.Del(name)
	}
}

func closeWrite(conn net.Conn) {
	if tcp, ok := conn.(*net.TCPConn); ok {
		_ = tcp.CloseWrite()
	}
}

func runSOCKS5Proxy() error {
	listener, err := net.Listen("tcp", socksProxyAddr)
	if err != nil {
		return err
	}
	log.Printf("SOCKS5 proxy listening on %s (TCP + UDP ASSOCIATE)", socksProxyAddr)

	for {
		conn, err := listener.Accept()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return err
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		go handleSOCKSClient(conn)
	}
}

func handleSOCKSClient(conn net.Conn) {
	defer conn.Close()
	_ = conn.SetDeadline(time.Now().Add(dialTimeout))

	reader := bufio.NewReader(conn)
	greeting := make([]byte, 2)
	if _, err := io.ReadFull(reader, greeting); err != nil || greeting[0] != 5 {
		return
	}
	methods := make([]byte, int(greeting[1]))
	if _, err := io.ReadFull(reader, methods); err != nil {
		return
	}
	if !containsByte(methods, 0x00) {
		_, _ = conn.Write([]byte{5, 0xFF})
		return
	}
	if _, err := conn.Write([]byte{5, 0x00}); err != nil {
		return
	}

	requestHeader := make([]byte, 4)
	if _, err := io.ReadFull(reader, requestHeader); err != nil || requestHeader[0] != 5 {
		return
	}
	host, port, err := readSOCKSAddr(reader, requestHeader[3])
	if err != nil {
		writeSOCKSReply(conn, 0x08, nil) // address type not supported
		return
	}

	switch requestHeader[1] {
	case 0x01: // CONNECT
		socksConnect(conn, reader, host, port)
	case 0x03: // UDP ASSOCIATE
		clientIP := net.IPv4zero
		if remote, ok := conn.RemoteAddr().(*net.TCPAddr); ok {
			clientIP = remote.IP
		}
		socksUDPAssociate(conn, reader, clientIP)
	default:
		writeSOCKSReply(conn, 0x07, nil) // command not supported
	}
}

func containsByte(haystack []byte, needle byte) bool {
	for _, value := range haystack {
		if value == needle {
			return true
		}
	}
	return false
}

func socksConnect(conn net.Conn, reader *bufio.Reader, host string, port uint16) {
	target := net.JoinHostPort(host, strconv.Itoa(int(port)))
	upstream, err := net.DialTimeout("tcp", target, dialTimeout)
	if err != nil {
		writeSOCKSReply(conn, 0x05, nil) // connection refused
		return
	}
	defer upstream.Close()

	writeSOCKSReply(conn, 0x00, upstream.LocalAddr())
	_ = conn.SetDeadline(time.Time{})

	go func() {
		_, _ = io.Copy(upstream, reader)
		closeWrite(upstream)
	}()
	_, _ = io.Copy(conn, upstream)
}

func socksUDPAssociate(conn net.Conn, reader *bufio.Reader, clientIP net.IP) {
	relay, err := net.ListenUDP("udp", &net.UDPAddr{})
	if err != nil {
		writeSOCKSReply(conn, 0x01, nil)
		return
	}
	defer relay.Close()

	local := relay.LocalAddr().(*net.UDPAddr)
	replyIP := local.IP
	if replyIP == nil || replyIP.IsUnspecified() {
		if tcpLocal, ok := conn.LocalAddr().(*net.TCPAddr); ok {
			replyIP = tcpLocal.IP
		}
	}
	writeSOCKSReply(conn, 0x00, &net.UDPAddr{IP: replyIP, Port: local.Port})
	_ = conn.SetDeadline(time.Time{})

	// The association lives as long as the TCP control connection.
	go func() {
		_, _ = io.Copy(io.Discard, reader)
		_ = relay.Close()
	}()

	clientAddr := &net.UDPAddr{IP: clientIP}
	buffer := make([]byte, 65535)
	for {
		n, source, err := relay.ReadFromUDP(buffer)
		if err != nil {
			return
		}

		fromClient := source.IP.Equal(clientIP) &&
			(clientAddr.Port == 0 || source.Port == clientAddr.Port)
		if fromClient {
			clientAddr = source
			destination, payload, err := parseUDPRequest(buffer[:n])
			if err != nil {
				continue
			}
			_, _ = relay.WriteToUDP(payload, destination)
			continue
		}

		if clientAddr.Port == 0 {
			continue
		}
		_, _ = relay.WriteToUDP(buildUDPResponse(source, buffer[:n]), clientAddr)
	}
}

func readSOCKSAddr(reader io.Reader, atyp byte) (string, uint16, error) {
	var host string
	switch atyp {
	case 0x01:
		raw := make([]byte, net.IPv4len)
		if _, err := io.ReadFull(reader, raw); err != nil {
			return "", 0, err
		}
		host = net.IP(raw).String()
	case 0x03:
		length := make([]byte, 1)
		if _, err := io.ReadFull(reader, length); err != nil {
			return "", 0, err
		}
		raw := make([]byte, int(length[0]))
		if _, err := io.ReadFull(reader, raw); err != nil {
			return "", 0, err
		}
		host = string(raw)
	case 0x04:
		raw := make([]byte, net.IPv6len)
		if _, err := io.ReadFull(reader, raw); err != nil {
			return "", 0, err
		}
		host = net.IP(raw).String()
	default:
		return "", 0, fmt.Errorf("unsupported address type 0x%02x", atyp)
	}

	rawPort := make([]byte, 2)
	if _, err := io.ReadFull(reader, rawPort); err != nil {
		return "", 0, err
	}
	return host, binary.BigEndian.Uint16(rawPort), nil
}

func writeSOCKSReply(writer io.Writer, reply byte, addr net.Addr) {
	packet := []byte{5, reply, 0}
	if addr == nil {
		packet = append(packet, 1, 0, 0, 0, 0, 0, 0)
		_, _ = writer.Write(packet)
		return
	}

	var ip net.IP
	var port int
	switch value := addr.(type) {
	case *net.TCPAddr:
		ip, port = value.IP, value.Port
	case *net.UDPAddr:
		ip, port = value.IP, value.Port
	}

	if ip4 := ip.To4(); ip4 != nil {
		packet = append(packet, 1)
		packet = append(packet, ip4...)
	} else if ip16 := ip.To16(); ip16 != nil {
		packet = append(packet, 4)
		packet = append(packet, ip16...)
	} else {
		packet = append(packet, 1, 0, 0, 0, 0)
		port = 0
	}
	packet = append(packet, byte(port>>8), byte(port))
	_, _ = writer.Write(packet)
}

// parseUDPRequest strips the SOCKS5 UDP header. FRAG != 0 is dropped because
// fragmentation is effectively unused by real clients.
func parseUDPRequest(packet []byte) (*net.UDPAddr, []byte, error) {
	if len(packet) < 4 || packet[0] != 0 || packet[1] != 0 {
		return nil, nil, errors.New("malformed UDP request")
	}
	if packet[2] != 0 {
		return nil, nil, errors.New("fragmented UDP not supported")
	}

	offset := 4
	var host string
	switch packet[3] {
	case 0x01:
		if len(packet) < offset+net.IPv4len+2 {
			return nil, nil, errors.New("short IPv4 UDP request")
		}
		host = net.IP(packet[offset : offset+net.IPv4len]).String()
		offset += net.IPv4len
	case 0x03:
		if len(packet) < offset+1 {
			return nil, nil, errors.New("short domain UDP request")
		}
		length := int(packet[offset])
		offset++
		if len(packet) < offset+length+2 {
			return nil, nil, errors.New("short domain UDP request")
		}
		host = string(packet[offset : offset+length])
		offset += length
	case 0x04:
		if len(packet) < offset+net.IPv6len+2 {
			return nil, nil, errors.New("short IPv6 UDP request")
		}
		host = net.IP(packet[offset : offset+net.IPv6len]).String()
		offset += net.IPv6len
	default:
		return nil, nil, errors.New("unsupported UDP address type")
	}

	port := int(binary.BigEndian.Uint16(packet[offset : offset+2]))
	offset += 2

	destination, err := net.ResolveUDPAddr("udp", net.JoinHostPort(host, strconv.Itoa(port)))
	if err != nil {
		return nil, nil, err
	}
	return destination, packet[offset:], nil
}

func buildUDPResponse(source *net.UDPAddr, payload []byte) []byte {
	packet := make([]byte, 0, 4+net.IPv6len+2+len(payload))
	packet = append(packet, 0, 0, 0)

	if ip4 := source.IP.To4(); ip4 != nil {
		packet = append(packet, 1)
		packet = append(packet, ip4...)
	} else {
		packet = append(packet, 4)
		packet = append(packet, source.IP.To16()...)
	}
	packet = append(packet, byte(source.Port>>8), byte(source.Port))
	return append(packet, payload...)
}
