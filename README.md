# Contiki-NG Üzerinde Güvenilir OTA Firmware Aktarımı Raporu

Bu rapor, Contiki-NG işletim sistemi üzerinde çalışan, Cooja simülatör ortamında test edilmek üzere tasarlanmış Stop-and-Wait (Dur-ve-Bekle) tabanlı Over-the-Air (OTA) firmware aktarım protokolünün tasarımını ve güvenilirlik önlemlerini içermektedir.

---

## 1. GİRİŞ VE TEORİK ALTYAPI

### 1.1. Stop-and-Wait Protokolü Mantığı
Stop-and-Wait (Dur-ve-Bekle) protokolü, veri iletiminde güvenilirliği (Reliability) ve akış kontrolünü (Flow Control) sağlamak amacıyla kullanılan en temel Otomatik Tekrar İsteği (ARQ - Automatic Repeat reQuest) mekanizmalarından biridir. Protokolün temel çalışma prensibi şu adımlardan oluşur:
1. **Gönderici (Sender)**, veriyi bir blok (paket) halinde alıcıya gönderir.
2. Gönderici, paketi yolladıktan sonra yeni bir veri göndermeyi durdurur ve alıcıdan onay (ACK - Acknowledgement) mesajı gelene kadar beklemeye başlar. Aynı zamanda bir zaman aşımı (Timeout) sayacı başlatır.
3. **Alıcı (Receiver)**, paketi aldığında bütünlük (Integrity) ve sıra kontrolü yapar. Eğer paket hatasız ve beklenen sırada ise paketi işler ve göndericiye ilgili blok numarasına ait bir **ACK** paketi gönderir.
4. **Zaman Aşımı (Timeout) Yönetimi**: Eğer gönderilen paket veya alıcının gönderdiği ACK yolda kaybolursa, göndericinin zamanlayıcısı aşılır. Zaman aşımı durumunda gönderici aynı paketi yeniden iletir (Retransmission).
5. **Kopya Paket (Duplicate) Yönetimi**: Alıcının ACK mesajı yolda kaybolursa, gönderici zaman aşımına uğrayıp aynı paketi tekrar gönderir. Alıcı, zaten almış olduğu bu paketi tekrar aldığını (paket sırasından) tespit eder; paketi işleme almadan (Flash belleğe yazmadan) drop eder ve göndericinin döngüde takılı kalmaması için ilgili bloğun ACK mesajını yeniden gönderir.

### 1.2. Paket Yapısı Tasarımı (64 Byte Payload)
Düşük güçlü ve kayıplı kablosuz sensör ağlarında (LLN - Low-power and Lossy Networks) IEEE 802.15.4 fiziksel katman standartları kullanılmaktadır. Bu standart altında maksimum iletim birimi (MTU) 127 byte ile sınırlıdır. IPv6 ve UDP başlıkları da eklendiğinde, paketlerin bölünmesini (fragmentation) engellemek amacıyla uygulama katmanı payload boyutunu optimize etmek kritik önem taşır.

Bu projede firmware aktarımı için `BLOCK_SIZE` **64 byte** olarak belirlenmiştir. Paket yapısı, C dilinde derleyicinin otomatik olarak hizalama (padding) yapmasını engellemek için `__attribute__((packed))` özniteliği ile sıkıştırılmıştır:

```c
#define BLOCK_SIZE 64

struct ota_packet {
  uint16_t block_id;             // Mevcut bloğun numarası (0'dan başlar)
  uint16_t total_blocks;         // Toplam blok sayısı
  uint8_t data_length;           // Paketteki gerçek veri miktarı (son blokta < 64 olabilir)
  uint8_t payload[BLOCK_SIZE];   // Gerçek firmware byte dizisi (64 byte)
  uint8_t checksum;              // Basit bütünlük kontrolü (XOR Checksum)
} __attribute__((packed));
```

### 1.3. XOR Checksum (Bütünlük Kontrolü) Teorisi
Hata algılama (Error Detection) amacıyla kullanılan XOR Checksum, paket içeriğindeki tüm byte'ların ardışık olarak Özel Veya (XOR - $\oplus$) mantıksal işlemine tabi tutulması esasına dayanır. 
Matematiksel olarak:
$$\text{Checksum} = B_0 \oplus B_1 \oplus B_2 \oplus \dots \oplus B_{N-1}$$
Burada $B_i$ pakette yer alan ve `checksum` alanı dışındaki her bir byte'ı temsil eder. 

**XOR Checksum Avantajları:**
- Düşük güçlü sensör düğümlerinde (MSP430 gibi kısıtlı donanımlar) işlemci ve bellek maliyeti neredeyse sıfıra yakındır. Tek çevrimlik mantıksal XOR komutlarıyla hesaplanır.
- Tek bitlik hataları ve tek sayıda bit değişimlerini mükemmel şekilde yakalar.

Projede kullanılan bütünlük doğrulama fonksiyonu:
```c
static inline uint8_t
calculate_checksum(const struct ota_packet *pkt)
{
  const uint8_t *ptr = (const uint8_t *)pkt;
  uint8_t xor_val = 0;
  size_t i;
  // Checksum alanı hariç (offsetof) tüm yapı XORlanır
  for(i = 0; i < offsetof(struct ota_packet, checksum); i++) {
    xor_val ^= ptr[i];
  }
  return xor_val;
}
```

---

## 2. SİSTEM MİMARİSİ VE KOD PARÇALARI

### 2.1. Gönderici Düğüm (udp-client.c - Node 2)
Gönderici düğüm, ağ bağlantısı sağlandığında ve RPL yönlendirme ağacı kurulduğunda (`node_is_reachable()`) OTA aktarımını başlatır. 

**İletim Akışı:**
- Toplam blok sayısı hesaplanır: `total_blocks = (firmware_data_len + BLOCK_SIZE - 1) / BLOCK_SIZE;`
- Sıradaki blok `firmware_data` içerisinden `payload` dizisine kopyalanır ve XOR checksum hesaplanarak alıcıya (`Node 1`) gönderilir.
- `waiting_for_ack` durumu `true` yapılarak zamanlayıcı (`etimer`) 2 saniyeye kurulur.
- ACK geldiğinde `udp_rx_callback` tetiklenir. Gelen ACK verisi beklenen `current_block_id` değerini içeriyorsa:
  - `current_block_id++` yapılır, `waiting_for_ack = false` durumuna geçilir.
  - `process_poll(&udp_client_process)` çağrılarak işlem döngüsü anında uyandırılır; böylece zaman aşımı süresinin bitmesi beklenmeden anında bir sonraki bloğun gönderimi başlatılır.

**Gönderici Ana Döngü Parçası:**
```c
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&periodic_timer) || ev == PROCESS_EVENT_POLL);

    if(node_id == 2) {
      if(!ota_started) {
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {
          LOG_INFO("Root is reachable. Starting OTA transmission of %u blocks.\n", total_blocks);
          ota_started = true;
          send_block(current_block_id);
          waiting_for_ack = true;
          etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
        } else {
          LOG_INFO("Not reachable yet\n");
          etimer_set(&periodic_timer, 5 * CLOCK_SECOND);
        }
      } else {
        if(current_block_id < total_blocks) {
          if(!waiting_for_ack) {
            send_block(current_block_id);
            waiting_for_ack = true;
            etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
          } else {
            if(etimer_expired(&periodic_timer)) {
              LOG_INFO("Timeout: Retransmitting block %u\n", current_block_id);
              send_block(current_block_id);
              etimer_set(&periodic_timer, TIMEOUT_INTERVAL);
            }
          }
        } else {
          LOG_INFO("OTA transmission completed. All %u blocks sent.\n", total_blocks);
          etimer_stop(&periodic_timer);
        }
      }
    }
  }
```

### 2.2. Alıcı Düğüm (udp-server.c - Node 1)
Alıcı düğüm, ağ kökü (RPL Root) olarak çalışır. Gelen paketleri karşılar ve Stop-and-Wait mantığına göre işler.

**Alıcı Callback Mantığı:**
- Gelen paket boyutu kontrol edilir (`datalen == sizeof(struct ota_packet)`).
- `calculate_checksum` fonksiyonu ile bütünlük doğrulanır. Hatalıysa paket düşürülür (drop).
- `pkt->block_id == expected_block_id` ise:
  - Kalıcı depolama simülasyonu olarak ekrana `"Flash belleğe yazılıyor..."` ifadesi yazdırılır.
  - `expected_block_id` bir artırılır.
  - İstemciye ilgili bloğun alındığına dair onay (`uint16_t ack_val`) gönderilir.
  - Son blok başarıyla işlendiğinde ekrana tam olarak şu log basılır:
    `"Yüklenmeye hazır yeni firmware alımı tamamlandı."`
- `pkt->block_id < expected_block_id` ise:
  - Göndericiye o blok için tekrar ACK gönderilir. Bu sayede kaybolan ACK durumlarında gönderici kilitlenmekten kurtulur.

**Alıcı Karşılayıcı Callback Kodu:**
```c
static void
udp_rx_callback(struct simple_udp_connection *c,
         const uip_ipaddr_t *sender_addr,
         uint16_t sender_port,
         const uip_ipaddr_t *receiver_addr,
         uint16_t receiver_port,
         const uint8_t *data,
         uint16_t datalen)
{
  if(datalen == sizeof(struct ota_packet)) {
    const struct ota_packet *pkt = (const struct ota_packet *)data;
    uint8_t computed_checksum = calculate_checksum(pkt);

    if(computed_checksum != pkt->checksum) {
      LOG_WARN("Checksum error! Packet dropped (expected: 0x%02x, calculated: 0x%02x).\n",
               pkt->checksum, computed_checksum);
      return;
    }

    if(pkt->block_id == expected_block_id) {
      LOG_INFO("Flash belleğe yazılıyor... (Block ID: %u)\n", pkt->block_id);
      expected_block_id++;

      uint16_t ack_val = pkt->block_id;
      simple_udp_sendto(&udp_conn, &ack_val, sizeof(ack_val), sender_addr);

      if(expected_block_id == pkt->total_blocks) {
        LOG_INFO("Yüklenmeye hazır yeni firmware alımı tamamlandı.\n");
      }
    } else if(pkt->block_id < expected_block_id) {
      LOG_INFO("Received duplicate block %u, expected %u. Resending ACK.\n",
               pkt->block_id, expected_block_id);
      uint16_t ack_val = pkt->block_id;
      simple_udp_sendto(&udp_conn, &ack_val, sizeof(ack_val), sender_addr);
    }
  }
}
