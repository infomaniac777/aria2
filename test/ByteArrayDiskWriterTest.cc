#include "ByteArrayDiskWriter.h"
#include <string>
#include <cppunit/extensions/HelperMacros.h>
#include "buffer.h"

namespace aria2 {

class ByteArrayDiskWriterTest : public CppUnit::TestFixture {

  CPPUNIT_TEST_SUITE(ByteArrayDiskWriterTest);
  CPPUNIT_TEST(testWriteData);
  CPPUNIT_TEST(testReadData);
  CPPUNIT_TEST_SUITE_END();

private:
public:
  void setUp() {}

  void testWriteData();
  void testReadData();
};

CPPUNIT_TEST_SUITE_REGISTRATION(ByteArrayDiskWriterTest);

void ByteArrayDiskWriterTest::testWriteData()
{
  ByteArrayDiskWriter bw;
  std::string msg1 = "hello";
  std::string msg2 = " world";
  std::string msg3 = "!";
  std::string msg4 = "H";
  
  auto buffer1 = buffer::create(msg1.size());
  std::copy(msg1.begin(), msg1.end(), buffer1->data());
  bw.writeData(buffer1, 0, msg1.size(), 0);
  
  auto buffer2 = buffer::create(msg2.size());
  std::copy(msg2.begin(), msg2.end(), buffer2->data());
  bw.writeData(buffer2, 0, msg2.size(), 5);
  
  auto buffer3 = buffer::create(msg3.size());
  std::copy(msg3.begin(), msg3.end(), buffer3->data());
  bw.writeData(buffer3, 0, msg3.size(), 12);
  
  auto buffer4 = buffer::create(msg4.size());
  std::copy(msg4.begin(), msg4.end(), buffer4->data());
  bw.writeData(buffer4, 0, msg4.size(), 11);
  
  CPPUNIT_ASSERT_EQUAL(std::string("hello world!"),
                       std::string(bw.getBytes().begin(), bw.getBytes().end()));
}

void ByteArrayDiskWriterTest::testReadData()
{
  ByteArrayDiskWriter bw;
  std::string msg1 = "hello ";
  std::string msg2 = "world";
  
  auto buffer1 = buffer::create(msg1.size());
  std::copy(msg1.begin(), msg1.end(), buffer1->data());
  bw.writeData(buffer1, 0, msg1.size(), 0);
  
  auto buffer2 = buffer::create(msg2.size());
  std::copy(msg2.begin(), msg2.end(), buffer2->data());
  bw.writeData(buffer2, 0, msg2.size(), 6);
  
  auto readBuffer = buffer::create(11);
  ssize_t r = bw.readData(readBuffer, 0, 11, 0);
  CPPUNIT_ASSERT_EQUAL((ssize_t)11, r);
  CPPUNIT_ASSERT_EQUAL(std::string("hello world"),
                       std::string(readBuffer->data(), readBuffer->data() + 11));
}

} // namespace aria2
