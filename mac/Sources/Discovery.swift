import Darwin
import Foundation

/// Lets the client find the Mac without knowing its address: a MG.discoverQuery datagram on the
/// server's port, typically sent to the all-nodes address ff02::1 of the Thunderbolt link, gets
/// "MACGLASS <protocol version> <computer name>" back, from the address the client should connect
/// to. Bound to the same interface as the server, so it isn't visible over Wi-Fi or Ethernet.
final class DiscoveryResponder {
    let port: UInt16
    let interface: String

    init(port: UInt16, interface: String) {
        self.port = port
        self.interface = interface
    }

    func start() throws {
        let fd = socket(AF_INET6, SOCK_DGRAM, 0)
        guard fd >= 0 else { throw MGError("discovery socket: \(String(cString: strerror(errno)))") }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, socklen_t(MemoryLayout<Int32>.size))
        var index = if_nametoindex(interface)
        guard index != 0 else { throw MGError("no network interface named \(interface)") }
        guard setsockopt(fd, IPPROTO_IPV6, IPV6_BOUND_IF, &index, socklen_t(MemoryLayout<UInt32>.size)) == 0 else {
            throw MGError("cannot bind discovery to \(interface): \(String(cString: strerror(errno)))")
        }
        var addr = sockaddr_in6()
        addr.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
        addr.sin6_family = sa_family_t(AF_INET6)
        addr.sin6_port = port.bigEndian
        addr.sin6_addr = in6addr_any
        let bound = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) }
        }
        guard bound == 0 else { throw MGError("cannot bind discovery port \(port): \(String(cString: strerror(errno)))") }
        // macOS delivers multicast only to sockets that joined the group, even the all-nodes one.
        var group = ipv6_mreq()
        inet_pton(AF_INET6, "ff02::1", &group.ipv6mr_multiaddr)
        group.ipv6mr_interface = index
        guard setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &group, socklen_t(MemoryLayout<ipv6_mreq>.size)) == 0 else {
            throw MGError("cannot join ff02::1 on \(interface): \(String(cString: strerror(errno)))")
        }
        let name = Host.current().localizedName ?? ProcessInfo.processInfo.hostName
        let reply = Array("\(MG.discoverReply) \(MG.protocolVersion) \(name)".utf8)
        let query = Array(MG.discoverQuery.utf8)
        Thread {
            var buffer = [UInt8](repeating: 0, count: 64)
            var from = sockaddr_storage()
            while true {
                var fromLength = socklen_t(MemoryLayout<sockaddr_storage>.size)
                let n = withUnsafeMutablePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, &buffer, buffer.count, 0, $0, &fromLength) }
                }
                guard n == query.count, Array(buffer[0..<n]) == query else { continue }
                _ = withUnsafePointer(to: &from) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(fd, reply, reply.count, 0, $0, fromLength) }
                }
            }
        }.start()
    }
}
