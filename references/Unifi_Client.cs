using System.Net;
using System.Reflection.Metadata;
using System.Text.Json;


namespace Unifi_Voucher
{

    internal class Unifi_Client
    {
        private CookieContainer _cookieContainer;
        private HttpClient _httpClient;
        private SettingsManager _settingsManager;
        public Unifi_Client()
        {
            // Initialize shared CookieContainer, HttpClientHandler, and HttpClient
            _cookieContainer = new CookieContainer();
            _settingsManager = new SettingsManager(@"SOFTWARE\Unifi_Voucher");
            var httpClientHandler = new HttpClientHandler
            {
                CookieContainer = _cookieContainer,
                ServerCertificateCustomValidationCallback = (message, cert, chain, sslPolicyErrors) => true
            };
            
            _httpClient = new HttpClient(httpClientHandler);
        }

        // Method for GET request using shared HttpClient
        public async Task<string> GetSecureDataAsync(string url)
        {
            var response = await _httpClient.GetAsync(url);
            response.EnsureSuccessStatusCode();
            return await response.Content.ReadAsStringAsync();
        }
        // Method for authenticated POST request using shared HttpClient
        private async Task<string> PostSecureDataAsync(string url, string jsonData)
        {
            StringContent content = new StringContent(jsonData, System.Text.Encoding.UTF8, "application/json");
            HttpResponseMessage response = await _httpClient.PostAsync(url, content);
            response.EnsureSuccessStatusCode();
            return await response.Content.ReadAsStringAsync();
        }
        
         public async Task<string> Login()
        {
            string Controller_Base = "https://" + _settingsManager.GetStringSetting("Controller_Address") + ":8443";
            string UserName = _settingsManager.GetStringSetting("UserName");
            string Password = _settingsManager.GetStringSetting("Password");
            string SiteName = _settingsManager.GetStringSetting("Sitename");

            string url = Controller_Base + "/api/login";
            string jsonData = "{\"username\":\"" + UserName + "\",\"password\":\"" + Password + "\",\"for_hotspot\":\"true\",\"remember\":\"false\",\"site_name\":\"" + SiteName + "\"}";
            try
            {
                string response = await PostSecureDataAsync(url, jsonData);
                //resultTextView.Text = $"POST Response:\n{response}";
                return response;
            }
            catch (Exception ex)
            {
                return $"Error: {ex.Message}";
            }
        }

         public async Task<string> OnIssueVoucher(string GuestName, string CompanyName, string timeinseconds)
        {
            if (GuestName != "" | CompanyName != "" | GuestName != "" && CompanyName != "")
            {
                string Controller_Base = "https://" + _settingsManager.GetStringSetting("Controller_Address") + ":8443";
                string TimeInSeconds = _settingsManager.GetStringSetting("TimeInSeconds");
                string UploadSpeed = _settingsManager.GetStringSetting("UploadSpeed");
                string DownloadSpeed = _settingsManager.GetStringSetting("DownloadSpeed");
                string note = GuestName + " - " + CompanyName;
                string url = Controller_Base + "/api/s/" + _settingsManager.GetStringSetting("Sitename") +"/cmd/hotspot";
                //string jsonData = "{\"quota\":1,\"up\":" + UploadSpeed + ",\"down\":100000,\"note\":\"test\",\"n\":1,\"expire_number\":24,\"expire_unit\":60,\"cmd\":\"create-voucher\"}";

                string jsonData = "{\"quota\":1,\"up\":"+ UploadSpeed +",\"down\":"+ DownloadSpeed +",\"note\":\""+ note + "\",\"n\":1,\"expire_number\":" + TimeInSeconds + ",\"expire_unit\":1,\"cmd\":\"create-voucher\"}";
                //string jsonData = "{\"quota\":1,\"note\":\"" + Description + "\",\"n\":1,\"expire_number\":" + TimeInSeconds + ",\"expire_unit\":1,\"cmd\":\"create-voucher\",\"up\":50000,\"down\":100000,}";
                try
                {
                    string response = await PostSecureDataAsync(url, jsonData);
                    using JsonDocument document = JsonDocument.Parse(response);
                    return document.RootElement.GetProperty("data")[0].GetProperty("create_time").GetInt64().ToString(); 
                }
                catch (Exception ex)
                {
                    return $"Error: {ex.Message}";
                }

            }
            else
            {
                return "missing context: Guest name or company name.";
            }

        }

        public async Task<string> OnRetrieveVoucher(string CreateTime)
        {
            if (CreateTime != "" )
            {
                string Controller_Base = "https://" + _settingsManager.GetStringSetting("Controller_Address") + ":8443/api/s/";
                string url = Controller_Base + _settingsManager.GetStringSetting("Sitename") + "/stat/voucher";
                string jsonData = "{\"create_time\":" + CreateTime + "}";
                try
                {
                    string response = await PostSecureDataAsync(url, jsonData);
                    using JsonDocument document = JsonDocument.Parse(response);
                    string Voucher_Code = document.RootElement.GetProperty("data")[0].GetProperty("code").ToString();
                    return Voucher_Code.Insert(5, "-");
                }
                catch (Exception ex)
                {
                    return $"Error: {ex.Message}";
                }

            }
            else
            {
                return "missing context: Create_Time";
            }

        }
        
    }
}
